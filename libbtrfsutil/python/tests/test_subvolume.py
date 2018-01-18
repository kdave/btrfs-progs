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

import fcntl
import errno
import os
import os.path
from pathlib import PurePath
import traceback

import btrfsutil
from tests import BtrfsTestCase, HAVE_PATH_LIKE


class TestSubvolume(BtrfsTestCase):
    def test_is_subvolume(self):
        dir = os.path.join(self.mountpoint, 'foo')
        os.mkdir(dir)

        for arg in self.path_or_fd(self.mountpoint):
            with self.subTest(type=type(arg)):
                self.assertTrue(btrfsutil.is_subvolume(arg))
        for arg in self.path_or_fd(dir):
            with self.subTest(type=type(arg)):
                self.assertFalse(btrfsutil.is_subvolume(arg))

        with self.assertRaises(btrfsutil.BtrfsUtilError) as e:
            btrfsutil.is_subvolume(os.path.join(self.mountpoint, 'bar'))
        # This is a bit of an implementation detail, but really this is testing
        # that the exception is initialized correctly.
        self.assertEqual(e.exception.btrfsutilerror, btrfsutil.ERROR_STATFS_FAILED)
        self.assertEqual(e.exception.errno, errno.ENOENT)

    def test_subvolume_id(self):
        dir = os.path.join(self.mountpoint, 'foo')
        os.mkdir(dir)

        for arg in self.path_or_fd(self.mountpoint):
            with self.subTest(type=type(arg)):
                self.assertEqual(btrfsutil.subvolume_id(arg), 5)
        for arg in self.path_or_fd(dir):
            with self.subTest(type=type(arg)):
                self.assertEqual(btrfsutil.subvolume_id(arg), 5)

    def test_create_subvolume(self):
        subvol = os.path.join(self.mountpoint, 'subvol')

        btrfsutil.create_subvolume(subvol + '1')
        self.assertTrue(btrfsutil.is_subvolume(subvol + '1'))
        btrfsutil.create_subvolume((subvol + '2').encode())
        self.assertTrue(btrfsutil.is_subvolume(subvol + '2'))
        if HAVE_PATH_LIKE:
            btrfsutil.create_subvolume(PurePath(subvol + '3'))
            self.assertTrue(btrfsutil.is_subvolume(subvol + '3'))

        pwd = os.getcwd()
        try:
            os.chdir(self.mountpoint)
            btrfsutil.create_subvolume('subvol4')
            self.assertTrue(btrfsutil.is_subvolume('subvol4'))
        finally:
            os.chdir(pwd)

        btrfsutil.create_subvolume(subvol + '5/')
        self.assertTrue(btrfsutil.is_subvolume(subvol + '5'))

        btrfsutil.create_subvolume(subvol + '6//')
        self.assertTrue(btrfsutil.is_subvolume(subvol + '6'))

        transid = btrfsutil.create_subvolume(subvol + '7', async=True)
        self.assertTrue(btrfsutil.is_subvolume(subvol + '7'))
        self.assertGreater(transid, 0)

        # Test creating subvolumes under '/' in a chroot.
        pid = os.fork()
        if pid == 0:
            try:
                os.chroot(self.mountpoint)
                os.chdir('/')
                btrfsutil.create_subvolume('/subvol8')
                self.assertTrue(btrfsutil.is_subvolume('/subvol8'))
                with self.assertRaises(btrfsutil.BtrfsUtilError):
                    btrfsutil.create_subvolume('/')
                os._exit(0)
            except Exception:
                traceback.print_exc()
                os._exit(1)
        wstatus = os.waitpid(pid, 0)[1]
        self.assertTrue(os.WIFEXITED(wstatus))
        self.assertEqual(os.WEXITSTATUS(wstatus), 0)
