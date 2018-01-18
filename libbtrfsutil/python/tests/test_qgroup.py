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
import unittest

import btrfsutil
from tests import BtrfsTestCase


class TestQgroup(BtrfsTestCase):
    def test_subvolume_inherit(self):
        subvol = os.path.join(self.mountpoint, 'subvol')

        inherit = btrfsutil.QgroupInherit()
        inherit.add_group(5)

        btrfsutil.create_subvolume(subvol, qgroup_inherit=inherit)

    def test_snapshot_inherit(self):
        subvol = os.path.join(self.mountpoint, 'subvol')
        snapshot = os.path.join(self.mountpoint, 'snapshot')

        inherit = btrfsutil.QgroupInherit()
        inherit.add_group(5)

        btrfsutil.create_subvolume(subvol)
        btrfsutil.create_snapshot(subvol, snapshot, qgroup_inherit=inherit)


class TestQgroupInherit(unittest.TestCase):
    def test_new(self):
        inherit = btrfsutil.QgroupInherit()
        self.assertEqual(inherit.groups, [])

    def test_add_group(self):
        inherit = btrfsutil.QgroupInherit()
        inherit.add_group(1)
        self.assertEqual(inherit.groups, [1])
        inherit.add_group(2)
        self.assertEqual(inherit.groups, [1, 2])
        inherit.add_group(3)
        self.assertEqual(inherit.groups, [1, 2, 3])
