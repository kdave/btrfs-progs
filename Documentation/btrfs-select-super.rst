btrfs-select-super(8)
=====================

SYNOPSIS
--------

**btrfs-select-super** -s number <device>

DESCRIPTION
-----------

Destructively overwrite all copies of the superblock with a specified copy.
This helps in certain cases, for example when write barriers were disabled
during a power failure and not all superblocks were written, or if the primary
superblock is damaged, eg. accidentally overwritten.

The filesystem specified by *device* must not be mounted.

.. note::
   Prior to overwriting the primary superblock, please make sure that the
   backup copies are valid!

To dump a superblock use the **btrfs inspect-internal dump-super** command.

Then run the check (in the non-repair mode) using the command **btrfs check -s**
where *-s* specifies the superblock copy to use.

Superblock copies exist in the following offsets on the device:

- primary: 64KiB (65536)
- 1st copy: 64MiB (67108864)
- 2nd copy: 256GiB (274877906944)

A superblock size is 4KiB (4096).

OPTIONS
-------

-s|--super <N>
        use Nth superblock copy, valid values are 0 1 or 2 if the
        respective superblock offset is within the device size

SEE ALSO
--------

btrfs(8)
