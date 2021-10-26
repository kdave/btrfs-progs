btrfs-map-logical(8)
====================

SYNOPSIS
--------

**btrfs-map-logical** <options> <device>

DESCRIPTION
-----------

**btrfs-map-logical** can be used to find out what the physical offsets are
on the mirrors, the result is dumped to stdout by default.

Mainly used for debug purpose.

OPTIONS
-------

-l|--logical <logical_num>
        Logical extent to map.
-c|--copy <copy>
        Copy of the extent to read(usually 1 or 2).
-o|--output <filename>
        Output file to hold the extent.
-b|--bytes <bytes>
        Number of bytes to read.

EXIT STATUS
-----------

**btrfs-map-logical** will return 0 if no error happened.
If any problems happened, 1 will be returned.

SEE ALSO
--------

``mkfs.btrfs(8)``
