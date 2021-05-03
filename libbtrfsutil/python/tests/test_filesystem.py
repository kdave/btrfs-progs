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

import os
import time

import btrfsutil
from tests import BtrfsTestCase, HAVE_PATH_LIKE


def touch(path):
    now = time.time()
    os.utime(path, (now, now))


class TestFilesystem(BtrfsTestCase):
    def super_generation(self):
        with open(self.image, 'rb') as f:
            # csum is 32 bytes, fsid is 16 bytes, bytenr is 8 bytes, flags is 8
            # bytes
            f.seek(65536 + 32 + 16 + 8 + 8)
            self.assertEqual(f.read(8), b'_BHRfS_M')
            return int.from_bytes(f.read(8), 'little')

    def test_sync(self):
        old_generation = self.super_generation()
        for arg in self.path_or_fd(self.mountpoint):
            with self.subTest(type=type(arg)):
                touch(arg)
                btrfsutil.sync(arg)
                new_generation = self.super_generation()
                self.assertGreater(new_generation, old_generation)
                old_generation = new_generation

    def test_start_sync(self):
        old_generation = self.super_generation()
        for arg in self.path_or_fd(self.mountpoint):
            with self.subTest(type=type(arg)):
                touch(arg)
                transid = btrfsutil.start_sync(arg)
                self.assertGreater(transid, old_generation)

    def test_wait_sync(self):
        old_generation = self.super_generation()
        for arg in self.path_or_fd(self.mountpoint):
            with self.subTest(type=type(arg)):
                touch(arg)
                transid = btrfsutil.start_sync(arg)
                btrfsutil.wait_sync(arg, transid)
                new_generation = self.super_generation()
                self.assertGreater(new_generation, old_generation)
                old_generation = new_generation

                touch(arg)
                btrfsutil.start_sync(arg)
                btrfsutil.wait_sync(arg)
                new_generation = self.super_generation()
                self.assertGreater(new_generation, old_generation)
                old_generation = new_generation
