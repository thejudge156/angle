//
// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// WorkerThread:
//   Task running thread for ANGLE, similar to a TaskRunner in Chromium.
//   Might be implemented differently depending on platform.
//

#include "libANGLE/WorkerThread.h"

#include "libANGLE/trace.h"

#if (ANGLE_DELEGATE_WORKERS == ANGLE_ENABLED) || (ANGLE_STD_ASYNC_WORKERS == ANGLE_ENABLED)
#    include <condition_variable>
#    include <future>
#    include <mutex>
#    include <queue>
#    include <thread>
#endif  // (ANGLE_DELEGATE_WORKERS == ANGLE_ENABLED) || (ANGLE_STD_ASYNC_WORKERS == ANGLE_ENABLED)

namespace angle
{

WaitableEvent::WaitableEvent()  = default;
WaitableEvent::~WaitableEvent() = default;

void WaitableEventDone::wait() {}

bool WaitableEventDone::isReady()
{
    return true;
}

// A waitable event that can be completed asynchronously
class AsyncWaitableEvent final : public WaitableEvent
{
  public:
    AsyncWaitableEvent()           = default;
    ~AsyncWaitableEvent() override = default;

    void wait() override;
    bool isReady() override;

    void markAsReady();

  private:
    // To protect the concurrent accesses from both main thread and background
    // threads to the member fields.
    std::mutex mMutex;

    bool mIsReady = false;
    std::condition_variable mCondition;
};

void AsyncWaitableEvent::markAsReady()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mIsReady = true;
    mCondition.notify_all();
}

void AsyncWaitableEvent::wait()
{
    std::unique_lock<std::mutex> lock(mMutex);
    mCondition.wait(lock, [this] { return mIsReady; });
}

bool AsyncWaitableEvent::isReady()
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mIsReady;
}

WorkerThreadPool::WorkerThreadPool()  = default;
WorkerThreadPool::~WorkerThreadPool() = default;

class SingleThreadedWorkerPool final : public WorkerThreadPool
{
  public:
    std::shared_ptr<WaitableEvent> postWorkerTask(std::shared_ptr<Closure> task) override;
    bool isAsync() override;
};

// SingleThreadedWorkerPool implementation.
std::shared_ptr<WaitableEvent> SingleThreadedWorkerPool::postWorkerTask(
    std::shared_ptr<Closure> task)
{
    (*task)();
    return std::make_shared<WaitableEventDone>();
}

bool SingleThreadedWorkerPool::isAsync()
{
    return false;
}

#if (ANGLE_STD_ASYNC_WORKERS == ANGLE_ENABLED)

class AsyncWorkerPool final : public WorkerThreadPool
{
  public:
    AsyncWorkerPool(size_t numThreads);

    ~AsyncWorkerPool() override;

    std::shared_ptr<WaitableEvent> postWorkerTask(std::shared_ptr<Closure> task) override;

    bool isAsync() override;

  private:
    using Task = std::pair<std::shared_ptr<AsyncWaitableEvent>, std::shared_ptr<Closure>>;

    // Thread's main loop
    void threadLoop();

    bool mTerminated = false;
    std::mutex mMutex;                 // Protects access to the fields in this class
    std::condition_variable mCondVar;  // Signals when work is available in the queue
    std::queue<Task> mTaskQueue;
    std::deque<std::thread> mThreads;
};

// AsyncWorkerPool implementation.

AsyncWorkerPool::AsyncWorkerPool(size_t numThreads)
{
    ASSERT(numThreads != 0);
    for (size_t i = 0; i < numThreads; ++i)
    {
        mThreads.emplace_back(&AsyncWorkerPool::threadLoop, this);
    }
}

AsyncWorkerPool::~AsyncWorkerPool()
{
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mTerminated = true;
    }
    mCondVar.notify_all();
    for (auto &thread : mThreads)
    {
        thread.join();
    }
}

std::shared_ptr<WaitableEvent> AsyncWorkerPool::postWorkerTask(std::shared_ptr<Closure> task)
{
    auto waitable = std::make_shared<AsyncWaitableEvent>();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mTaskQueue.push(std::make_pair(waitable, task));
    }
    mCondVar.notify_one();
    return std::move(waitable);
}

void AsyncWorkerPool::threadLoop()
{
    while (true)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mCondVar.wait(lock, [this] { return !mTaskQueue.empty() || mTerminated; });
            if (mTerminated)
            {
                return;
            }
            task = mTaskQueue.front();
            mTaskQueue.pop();
        }

        auto &waitable = task.first;
        auto &closure  = task.second;

        ANGLE_TRACE_EVENT0("gpu.angle", "AsyncWorkerPool::RunTask");
        (*closure)();
        waitable->markAsReady();
    }
}

bool AsyncWorkerPool::isAsync()
{
    return true;
}

#endif  // (ANGLE_STD_ASYNC_WORKERS == ANGLE_ENABLED)

#if (ANGLE_DELEGATE_WORKERS == ANGLE_ENABLED)

class DelegateWorkerPool final : public WorkerThreadPool
{
  public:
    DelegateWorkerPool()           = default;
    ~DelegateWorkerPool() override = default;

    std::shared_ptr<WaitableEvent> postWorkerTask(std::shared_ptr<Closure> task) override;

    bool isAsync() override;
};

// A function wrapper to execute the closure and to notify the waitable
// event after the execution.
class DelegateWorkerTask
{
  public:
    DelegateWorkerTask(std::shared_ptr<Closure> task, std::shared_ptr<AsyncWaitableEvent> waitable)
        : mTask(task), mWaitable(waitable)
    {}
    DelegateWorkerTask()                     = delete;
    DelegateWorkerTask(DelegateWorkerTask &) = delete;

    static void RunTask(void *userData)
    {
        DelegateWorkerTask *workerTask = static_cast<DelegateWorkerTask *>(userData);
        (*workerTask->mTask)();
        workerTask->mWaitable->markAsReady();

        // Delete the task after its execution.
        delete workerTask;
    }

  private:
    ~DelegateWorkerTask() = default;

    std::shared_ptr<Closure> mTask;
    std::shared_ptr<AsyncWaitableEvent> mWaitable;
};

std::shared_ptr<WaitableEvent> DelegateWorkerPool::postWorkerTask(std::shared_ptr<Closure> task)
{
    auto waitable = std::make_shared<AsyncWaitableEvent>();

    // The task will be deleted by DelegateWorkerTask::RunTask(...) after its execution.
    DelegateWorkerTask *workerTask = new DelegateWorkerTask(task, waitable);
    auto *platform                 = ANGLEPlatformCurrent();
    platform->postWorkerTask(platform, DelegateWorkerTask::RunTask, workerTask);

    return std::move(waitable);
}

bool DelegateWorkerPool::isAsync()
{
    return true;
}
#endif

// static
std::shared_ptr<WorkerThreadPool> WorkerThreadPool::Create(size_t numThreads)
{
    const bool multithreaded = numThreads != 1;
    std::shared_ptr<WorkerThreadPool> pool(nullptr);

#if (ANGLE_DELEGATE_WORKERS == ANGLE_ENABLED)
    const bool hasPostWorkerTaskImpl = ANGLEPlatformCurrent()->postWorkerTask;
    if (hasPostWorkerTaskImpl && multithreaded)
    {
        pool = std::shared_ptr<WorkerThreadPool>(new DelegateWorkerPool());
    }
#endif
#if (ANGLE_STD_ASYNC_WORKERS == ANGLE_ENABLED)
    if (!pool && multithreaded)
    {
        pool = std::shared_ptr<WorkerThreadPool>(new AsyncWorkerPool(
            numThreads == 0 ? std::thread::hardware_concurrency() : numThreads));
    }
#endif
    if (!pool)
    {
        return std::shared_ptr<WorkerThreadPool>(new SingleThreadedWorkerPool());
    }
    return pool;
}

// static
std::shared_ptr<WaitableEvent> WorkerThreadPool::PostWorkerTask(
    std::shared_ptr<WorkerThreadPool> pool,
    std::shared_ptr<Closure> task)
{
    std::shared_ptr<WaitableEvent> event = pool->postWorkerTask(task);
    if (event.get())
    {
        event->setWorkerThreadPool(pool);
    }
    return event;
}

}  // namespace angle
