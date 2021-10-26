btrfs-find-root(8)
==================

SYNOPSIS
--------

**btrfs-find-root** [options] <device>

DESCRIPTION
-----------

**btrfs-find-root** is used to find the satisfied root, you can filter by
root tree's objectid, generation, level.

OPTIONS
-------

-a
        Search through all metadata extents, even the root has been already found.
-g <generation>
        Filter root tree by it's original transaction id, tree root's generation in default.
-o <objectid>
        Filter root tree by it's objectid,tree root's objectid in default.
-l <level>
        Filter root tree by b-tree's level, level 0 in default.

EXIT STATUS
-----------

**btrfs-find-root** will return 0 if no error happened.
If any problems happened, 1 will be returned.

SEE ALSO
--------

``mkfs.btrfs(8)``
