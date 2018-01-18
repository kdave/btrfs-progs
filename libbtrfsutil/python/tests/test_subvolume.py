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

    def test_subvolume_path(self):
        btrfsutil.create_subvolume(os.path.join(self.mountpoint, 'subvol1'))
        os.mkdir(os.path.join(self.mountpoint, 'dir1'))
        os.mkdir(os.path.join(self.mountpoint, 'dir1/dir2'))
        btrfsutil.create_subvolume(os.path.join(self.mountpoint, 'dir1/dir2/subvol2'))
        btrfsutil.create_subvolume(os.path.join(self.mountpoint, 'dir1/dir2/subvol2/subvol3'))
        os.mkdir(os.path.join(self.mountpoint, 'subvol1/dir3'))
        btrfsutil.create_subvolume(os.path.join(self.mountpoint, 'subvol1/dir3/subvol4'))

        for arg in self.path_or_fd(self.mountpoint):
            with self.subTest(type=type(arg)):
                self.assertEqual(btrfsutil.subvolume_path(arg), '')
                self.assertEqual(btrfsutil.subvolume_path(arg, 5), '')
                self.assertEqual(btrfsutil.subvolume_path(arg, 256), 'subvol1')
                self.assertEqual(btrfsutil.subvolume_path(arg, 257), 'dir1/dir2/subvol2')
                self.assertEqual(btrfsutil.subvolume_path(arg, 258), 'dir1/dir2/subvol2/subvol3')
                self.assertEqual(btrfsutil.subvolume_path(arg, 259), 'subvol1/dir3/subvol4')

        pwd = os.getcwd()
        try:
            os.chdir(self.mountpoint)
            path = ''
            for i in range(26):
                name = chr(ord('a') + i) * 255
                path = os.path.join(path, name)
                btrfsutil.create_subvolume(name)
                os.chdir(name)
            self.assertEqual(btrfsutil.subvolume_path('.'), path)
        finally:
            os.chdir(pwd)

    def test_subvolume_info(self):
        for arg in self.path_or_fd(self.mountpoint):
            with self.subTest(type=type(arg)):
                info = btrfsutil.subvolume_info(arg)
                self.assertEqual(info.id, 5)
                self.assertEqual(info.parent_id, 0)
                self.assertEqual(info.dir_id, 0)
                self.assertEqual(info.flags, 0)
                self.assertEqual(info.uuid, bytes(16))
                self.assertEqual(info.parent_uuid, bytes(16))
                self.assertEqual(info.received_uuid, bytes(16))
                self.assertNotEqual(info.generation, 0)
                self.assertEqual(info.ctransid, 0)
                self.assertEqual(info.otransid, 0)
                self.assertEqual(info.stransid, 0)
                self.assertEqual(info.rtransid, 0)
                self.assertEqual(info.ctime, 0)
                self.assertEqual(info.otime, 0)
                self.assertEqual(info.stime, 0)
                self.assertEqual(info.rtime, 0)

        subvol = os.path.join(self.mountpoint, 'subvol')
        btrfsutil.create_subvolume(subvol)

        info = btrfsutil.subvolume_info(subvol)
        self.assertEqual(info.id, 256)
        self.assertEqual(info.parent_id, 5)
        self.assertEqual(info.dir_id, 256)
        self.assertEqual(info.flags, 0)
        self.assertIsInstance(info.uuid, bytes)
        self.assertEqual(info.parent_uuid, bytes(16))
        self.assertEqual(info.received_uuid, bytes(16))
        self.assertNotEqual(info.generation, 0)
        self.assertNotEqual(info.ctransid, 0)
        self.assertNotEqual(info.otransid, 0)
        self.assertEqual(info.stransid, 0)
        self.assertEqual(info.rtransid, 0)
        self.assertNotEqual(info.ctime, 0)
        self.assertNotEqual(info.otime, 0)
        self.assertEqual(info.stime, 0)
        self.assertEqual(info.rtime, 0)

        subvol_uuid = info.uuid
        snapshot = os.path.join(self.mountpoint, 'snapshot')
        btrfsutil.create_snapshot(subvol, snapshot)

        info = btrfsutil.subvolume_info(snapshot)
        self.assertEqual(info.parent_uuid, subvol_uuid)

        # TODO: test received_uuid, stransid, rtransid, stime, and rtime

        for arg in self.path_or_fd(self.mountpoint):
            with self.subTest(type=type(arg)):
                with self.assertRaises(btrfsutil.BtrfsUtilError) as e:
                    # BTRFS_EXTENT_TREE_OBJECTID
                    btrfsutil.subvolume_info(arg, 2)

    def test_read_only(self):
        for arg in self.path_or_fd(self.mountpoint):
            with self.subTest(type=type(arg)):
                btrfsutil.set_subvolume_read_only(arg)
                self.assertTrue(btrfsutil.get_subvolume_read_only(arg))
                self.assertTrue(btrfsutil.subvolume_info(arg).flags & 1)

                btrfsutil.set_subvolume_read_only(arg, False)
                self.assertFalse(btrfsutil.get_subvolume_read_only(arg))
                self.assertFalse(btrfsutil.subvolume_info(arg).flags & 1)

                btrfsutil.set_subvolume_read_only(arg, True)
                self.assertTrue(btrfsutil.get_subvolume_read_only(arg))
                self.assertTrue(btrfsutil.subvolume_info(arg).flags & 1)

                btrfsutil.set_subvolume_read_only(arg, False)

    def test_default_subvolume(self):
        for arg in self.path_or_fd(self.mountpoint):
            with self.subTest(type=type(arg)):
                self.assertEqual(btrfsutil.get_default_subvolume(arg), 5)

        subvol = os.path.join(self.mountpoint, 'subvol')
        btrfsutil.create_subvolume(subvol)
        for arg in self.path_or_fd(subvol):
            with self.subTest(type=type(arg)):
                btrfsutil.set_default_subvolume(arg)
                self.assertEqual(btrfsutil.get_default_subvolume(arg), 256)
                btrfsutil.set_default_subvolume(arg, 5)
                self.assertEqual(btrfsutil.get_default_subvolume(arg), 5)

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

    def test_create_snapshot(self):
        subvol = os.path.join(self.mountpoint, 'subvol')

        btrfsutil.create_subvolume(subvol)
        os.mkdir(os.path.join(subvol, 'dir'))

        for i, arg in enumerate(self.path_or_fd(subvol)):
            with self.subTest(type=type(arg)):
                snapshots_dir = os.path.join(self.mountpoint, 'snapshots{}'.format(i))
                os.mkdir(snapshots_dir)
                snapshot = os.path.join(snapshots_dir, 'snapshot')

                btrfsutil.create_snapshot(subvol, snapshot + '1')
                self.assertTrue(btrfsutil.is_subvolume(snapshot + '1'))
                self.assertTrue(os.path.exists(os.path.join(snapshot + '1', 'dir')))

                btrfsutil.create_snapshot(subvol, (snapshot + '2').encode())
                self.assertTrue(btrfsutil.is_subvolume(snapshot + '2'))
                self.assertTrue(os.path.exists(os.path.join(snapshot + '2', 'dir')))

                if HAVE_PATH_LIKE:
                    btrfsutil.create_snapshot(subvol, PurePath(snapshot + '3'))
                    self.assertTrue(btrfsutil.is_subvolume(snapshot + '3'))
                    self.assertTrue(os.path.exists(os.path.join(snapshot + '3', 'dir')))

        nested_subvol = os.path.join(subvol, 'nested')
        more_nested_subvol = os.path.join(nested_subvol, 'more_nested')
        btrfsutil.create_subvolume(nested_subvol)
        btrfsutil.create_subvolume(more_nested_subvol)
        os.mkdir(os.path.join(more_nested_subvol, 'nested_dir'))

        snapshot = os.path.join(self.mountpoint, 'snapshot')

        btrfsutil.create_snapshot(subvol, snapshot + '1')
        # Dummy subvolume.
        self.assertEqual(os.stat(os.path.join(snapshot + '1', 'nested')).st_ino, 2)
        self.assertFalse(os.path.exists(os.path.join(snapshot + '1', 'nested', 'more_nested')))

        btrfsutil.create_snapshot(subvol, snapshot + '2', recursive=True)
        self.assertTrue(os.path.exists(os.path.join(snapshot + '2', 'nested/more_nested/nested_dir')))

        transid = btrfsutil.create_snapshot(subvol, snapshot + '3', recursive=True, async=True)
        self.assertTrue(os.path.exists(os.path.join(snapshot + '3', 'nested/more_nested/nested_dir')))
        self.assertGreater(transid, 0)

        btrfsutil.create_snapshot(subvol, snapshot + '4', read_only=True)
        self.assertTrue(btrfsutil.get_subvolume_read_only(snapshot + '4'))

    def test_delete_subvolume(self):
        subvol = os.path.join(self.mountpoint, 'subvol')
        btrfsutil.create_subvolume(subvol + '1')
        self.assertTrue(os.path.exists(subvol + '1'))
        btrfsutil.create_subvolume(subvol + '2')
        self.assertTrue(os.path.exists(subvol + '2'))
        btrfsutil.create_subvolume(subvol + '3')
        self.assertTrue(os.path.exists(subvol + '3'))

        btrfsutil.delete_subvolume(subvol + '1')
        self.assertFalse(os.path.exists(subvol + '1'))
        btrfsutil.delete_subvolume((subvol + '2').encode())
        self.assertFalse(os.path.exists(subvol + '2'))
        if HAVE_PATH_LIKE:
            btrfsutil.delete_subvolume(PurePath(subvol + '3'))
            self.assertFalse(os.path.exists(subvol + '3'))

        # Test deleting subvolumes under '/' in a chroot.
        pid = os.fork()
        if pid == 0:
            try:
                os.chroot(self.mountpoint)
                os.chdir('/')
                btrfsutil.create_subvolume('/subvol4')
                self.assertTrue(os.path.exists('/subvol4'))
                btrfsutil.delete_subvolume('/subvol4')
                self.assertFalse(os.path.exists('/subvol4'))
                with self.assertRaises(btrfsutil.BtrfsUtilError):
                    btrfsutil.delete_subvolume('/')
                os._exit(0)
            except Exception:
                traceback.print_exc()
                os._exit(1)
        wstatus = os.waitpid(pid, 0)[1]
        self.assertTrue(os.WIFEXITED(wstatus))
        self.assertEqual(os.WEXITSTATUS(wstatus), 0)

        btrfsutil.create_subvolume(subvol + '5')
        btrfsutil.create_subvolume(subvol + '5/foo')
        btrfsutil.create_subvolume(subvol + '5/bar')
        btrfsutil.create_subvolume(subvol + '5/bar/baz')
        btrfsutil.create_subvolume(subvol + '5/bar/qux')
        btrfsutil.create_subvolume(subvol + '5/quux')
        with self.assertRaises(btrfsutil.BtrfsUtilError):
            btrfsutil.delete_subvolume(subvol + '5')
        btrfsutil.delete_subvolume(subvol + '5', recursive=True)
        self.assertFalse(os.path.exists(subvol + '5'))

    def test_subvolume_iterator(self):
        pwd = os.getcwd()
        try:
            os.chdir(self.mountpoint)
            btrfsutil.create_subvolume('foo')

            path, subvol = next(btrfsutil.SubvolumeIterator('.', info=True))
            self.assertEqual(path, 'foo')
            self.assertIsInstance(subvol, btrfsutil.SubvolumeInfo)
            self.assertEqual(subvol.id, 256)
            self.assertEqual(subvol.parent_id, 5)

            btrfsutil.create_subvolume('foo/bar')
            btrfsutil.create_subvolume('foo/bar/baz')

            subvols = [
                ('foo', 256),
                ('foo/bar', 257),
                ('foo/bar/baz', 258),
            ]

            for arg in self.path_or_fd('.'):
                with self.subTest(type=type(arg)):
                    self.assertEqual(list(btrfsutil.SubvolumeIterator(arg)), subvols)
            self.assertEqual(list(btrfsutil.SubvolumeIterator('.', top=0)), subvols)

            self.assertEqual(list(btrfsutil.SubvolumeIterator('.', post_order=True)),
                             [('foo/bar/baz', 258),
                              ('foo/bar', 257),
                              ('foo', 256)])

            subvols = [
                ('bar', 257),
                ('bar/baz', 258),
            ]

            self.assertEqual(list(btrfsutil.SubvolumeIterator('.', top=256)), subvols)
            self.assertEqual(list(btrfsutil.SubvolumeIterator('foo', top=0)), subvols)

            os.rename('foo/bar/baz', 'baz')
            self.assertEqual(sorted(btrfsutil.SubvolumeIterator('.')),
                             [('baz', 258),
                              ('foo', 256),
                              ('foo/bar', 257)])

            with btrfsutil.SubvolumeIterator('.') as it:
                self.assertGreaterEqual(it.fileno(), 0)
                it.close()
                with self.assertRaises(ValueError):
                    next(iter(it))
                with self.assertRaises(ValueError):
                    it.fileno()
                it.close()
        finally:
            os.chdir(pwd)
