#!/usr/bin/python3
# Copyright 2019 The ANGLE Project Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# gen_mtl_internal_shaders.py:
#   Code generation for Metal backend's default shaders.
#   NOTE: don't run this script directly. Run scripts/run_code_generation.py.

import json
import os
import subprocess
import sys

sys.path.append('../..')
import angle_format
import gen_angle_format_table

template_header_boilerplate = """// GENERATED FILE - DO NOT EDIT.
// Generated by {script_name}
//
// Copyright 2020 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
"""

def gen_shader_enums_code(angle_formats):

    code = """// This file is similar to src/libANGLE/renderer/FormatID_autogen.h but is used by Metal default
// shaders instead of C++ code.
//
"""

    code += "namespace rx\n"
    code += "{\n"
    code += "namespace mtl_shader\n"
    code += "{\n"
    code += "\n"
    code += "namespace FormatID\n"
    code += "{\n"
    code += "enum\n"
    code += "{\n"
    code += gen_angle_format_table.gen_enum_string(angle_formats) + '\n'
    code += "};\n\n"
    code += "}\n"
    code += "\n"
    code += "}\n"
    code += "}\n"

    return code


def find_clang():
    if os.name == 'nt':
        binary = 'clang-cl.exe'
    else:
        binary = 'clang++'

    clang = os.path.join('..', '..', '..', '..', '..', 'third_party', 'llvm-build',
                         'Release+Asserts', 'bin', binary)

    if not os.path.isfile(clang):
        xcrun_clang = subprocess.run(["xcrun", "-f", binary], stdout=subprocess.PIPE, text=True)
        if xcrun_clang.returncode == 0:
            clang = xcrun_clang.stdout.strip()
    if (not os.path.isfile(clang)):
        raise Exception('Cannot find clang')

    return clang


def main():
    angle_format_script_files = [
        '../../angle_format_map.json', '../../angle_format.py', '../../gen_angle_format_table.py'
    ]
    src_files = [
        'blit.metal', 'clear.metal', 'gen_indices.metal', 'gen_mipmap.metal', 'copy_buffer.metal',
        'visibility.metal', 'rewrite_indices.metal'
    ]

    # auto_script parameters.
    if len(sys.argv) > 1:
        inputs = angle_format_script_files + src_files + ['common.h', 'constants.h']
        outputs = ['format_autogen.h', 'mtl_default_shaders_src_autogen.inc']

        if sys.argv[1] == 'inputs':
            print(','.join(inputs))
        elif sys.argv[1] == 'outputs':
            print(','.join(outputs))
        else:
            print('Invalid script parameters')
            return 1
        return 0

    os.chdir(sys.path[0])

    boilerplate_code = template_header_boilerplate.format(
        script_name=os.path.basename(sys.argv[0]))

    # -------- Generate shader constants -----------
    angle_to_gl = angle_format.load_inverse_table('../../angle_format_map.json')
    shader_formats_autogen = gen_shader_enums_code(angle_to_gl.keys())
    shader_autogen_header = boilerplate_code + shader_formats_autogen

    with open('format_autogen.h', 'wt') as out_file:
        out_file.write(shader_autogen_header)
        out_file.close()

    # -------- Combine and create shader source string -----------
    # Generate combined source
    clang = find_clang()

    # Use clang to preprocess the combination source. "@@" token is used to prevent clang from
    # expanding the preprocessor directive
    temp_fname = 'temp_master_source.metal'
    with open(temp_fname, 'wb') as temp_file:
        for src_file in src_files:
            include_str = '#include "' + src_file + '" \n'
            temp_file.write(include_str.encode('utf-8'))

    args = [clang]
    if not os.name == 'nt':
        args += ['-xc++']
    args += ['-E', temp_fname]

    combined_source = subprocess.check_output(args)

    # Remove '@@' tokens
    final_combined_src_string = combined_source.replace('@@'.encode('utf-8'), ''.encode('utf-8'))

    # Generate final file:
    with open('mtl_default_shaders_src_autogen.inc', 'wt') as out_file:
        out_file.write(boilerplate_code)
        out_file.write('\n')
        out_file.write('// C++ string version of combined Metal default shaders.\n\n')
        out_file.write('\n\nstatic char gDefaultMetallibSrc[] = R"(\n')
        out_file.write(final_combined_src_string.decode("utf-8"))
        out_file.write('\n')
        out_file.write(')";\n')
        out_file.close()

    with open('mtl_default_shaders_src_autogen.metal', 'wt') as out_file:
        out_file.write(boilerplate_code)
        out_file.write('\n')
        out_file.write('// Metal version of combined Metal default shaders.\n\n')
        out_file.write(final_combined_src_string.decode("utf-8"))
        out_file.close()

    os.remove(temp_fname)


if __name__ == '__main__':
    sys.exit(main())
