#!/usr/bin/env python3

# Copyright (C) 2018 Facebook
#
# This file is part of libbtrfsutil.
#
# libbtrfsutil is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# libbtrfsutil is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with libbtrfsutil.  If not, see <http://www.gnu.org/licenses/>.

import re
import os
import os.path
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
import subprocess


def get_version():
    f = open('../../VERSION', 'r')
    version = f.readline().strip()
    f.close()
    return ".".join(version[1:].split('.'))


def out_of_date(dependencies, target):
    dependency_mtimes = [os.path.getmtime(dependency) for dependency in dependencies]
    try:
        target_mtime = os.path.getmtime(target)
    except OSError:
        return True
    return any(dependency_mtime >= target_mtime for dependency_mtime in dependency_mtimes)


def gen_constants():
    with open('../btrfsutil.h', 'r') as f:
        btrfsutil_h = f.read()

    constants = re.findall(
        r'^\s*(BTRFS_UTIL_ERROR_[a-zA-Z0-9_]+)',
        btrfsutil_h, flags=re.MULTILINE)

    with open('constants.c', 'w') as f:
        f.write("""\
#include <btrfsutil.h>
#include "btrfsutilpy.h"

void add_module_constants(PyObject *m)
{
""")
        for constant in constants:
            assert constant.startswith('BTRFS_UTIL_')
            name = constant[len('BTRFS_UTIL_'):]
            f.write('\tPyModule_AddIntConstant(m, "{}", {});\n'.format(name, constant))
        f.write("""\
}
""")


class my_build_ext(build_ext):
    def run(self):
        if out_of_date(['../btrfsutil.h'], 'constants.c'):
            try:
                gen_constants()
            except Exception as e:
                try:
                    os.remove('constants.c')
                except OSError:
                    pass
                raise e
        super().run()


module = Extension(
    name='btrfsutil',
    sources=[
        'constants.c',
        'error.c',
        'filesystem.c',
        'module.c',
        'qgroup.c',
        'subvolume.c',
    ],
    include_dirs=['..'],
    library_dirs=['../..'],
    libraries=['btrfsutil'],
)

setup(
    name='btrfsutil',
    version=get_version(),
    description='Library for managing Btrfs filesystems',
    url='https://github.com/kdave/btrfs-progs',
    license='LGPLv3',
    cmdclass={'build_ext': my_build_ext},
    ext_modules=[module],
)
