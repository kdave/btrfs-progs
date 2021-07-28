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

import contextlib
import os
from pathlib import PurePath
import pwd
import subprocess
import tempfile
import unittest


HAVE_PATH_LIKE = hasattr(PurePath, '__fspath__')
try:
    NOBODY_UID = pwd.getpwnam('nobody').pw_uid
    skipUnlessHaveNobody = lambda func: func
except KeyError:
    NOBODY_UID = None
    skipUnlessHaveNobody = unittest.skip('must have nobody user')


@contextlib.contextmanager
def drop_privs():
    try:
        os.seteuid(NOBODY_UID)
        yield
    finally:
        os.seteuid(0)


@contextlib.contextmanager
def regain_privs():
    uid = os.geteuid()
    if uid:
        try:
            os.seteuid(0)
            yield
        finally:
            os.seteuid(uid)
    else:
        yield


@unittest.skipIf(os.geteuid() != 0, 'must be run as root')
class BtrfsTestCase(unittest.TestCase):
    def __init__(self, *args, **kwds):
        super().__init__(*args, **kwds)
        self._mountpoints = []

    def mount_btrfs(self):
        mountpoint = tempfile.mkdtemp()
        try:
            with tempfile.NamedTemporaryFile(delete=False) as f:
                os.truncate(f.fileno(), 1024 * 1024 * 1024)
                image = f.name
        except Exception as e:
            os.rmdir(mountpoint)
            raise e

        if os.path.exists('../../mkfs.btrfs'):
            mkfs = '../../mkfs.btrfs'
        else:
            mkfs = 'mkfs.btrfs'
        try:
            subprocess.check_call([mkfs, '-q', image])
            subprocess.check_call(
                [
                    'mount',
                    '-o',
                    'loop,user_subvol_rm_allowed',
                    '--',
                    image,
                    mountpoint,
                ]
            )
        except Exception as e:
            os.rmdir(mountpoint)
            os.remove(image)
            raise e

        self._mountpoints.append((mountpoint, image))
        return mountpoint, image

    def setUp(self):
        self.mountpoint, self.image = self.mount_btrfs()

    def tearDown(self):
        for mountpoint, image in self._mountpoints:
            subprocess.call(['umount', '-R', mountpoint])
            os.rmdir(mountpoint)
            os.remove(image)

    @staticmethod
    def path_or_fd(path, open_flags=os.O_RDONLY):
        yield path
        yield path.encode()
        if HAVE_PATH_LIKE:
            yield PurePath(path)
        fd = os.open(path, open_flags)
        try:
            yield fd
        finally:
            os.close(fd)
