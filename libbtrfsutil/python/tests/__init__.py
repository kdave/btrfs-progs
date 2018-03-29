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
from pathlib import PurePath
import subprocess
import tempfile
import unittest


HAVE_PATH_LIKE = hasattr(PurePath, '__fspath__')


@unittest.skipIf(os.geteuid() != 0, 'must be run as root')
class BtrfsTestCase(unittest.TestCase):
    def setUp(self):
        self.mountpoint = tempfile.mkdtemp()
        try:
            with tempfile.NamedTemporaryFile(delete=False) as f:
                os.truncate(f.fileno(), 1024 * 1024 * 1024)
                self.image = f.name
        except Exception as e:
            os.rmdir(self.mountpoint)
            raise e

        if os.path.exists('../../mkfs.btrfs'):
            mkfs = '../../mkfs.btrfs'
        else:
            mkfs = 'mkfs.btrfs'
        try:
            subprocess.check_call([mkfs, '-q', self.image])
            subprocess.check_call(['mount', '-o', 'loop', '--', self.image, self.mountpoint])
        except Exception as e:
            os.remove(self.image)
            os.rmdir(self.mountpoint)
            raise e

    def tearDown(self):
        try:
            subprocess.check_call(['umount', self.mountpoint])
        finally:
            os.remove(self.image)
            os.rmdir(self.mountpoint)

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

