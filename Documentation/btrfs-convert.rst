btrfs-convert(8)
================

SYNOPSIS
--------

**btrfs-convert** [options] <device>

DESCRIPTION
-----------

.. include:: ch-convert-intro.rst

OPTIONS
-------

--csum <type>, --checksum <type>
        Specify the checksum algorithm. Default is *crc32c*. Valid values are *crc32c*,
        *xxhash*, *sha256* or *blake2*. To mount such filesystem kernel must support the
        checksums as well.

-d|--no-datasum
        disable data checksum calculations and set the NODATASUM file flag, this can speed
        up the conversion
-i|--no-xattr
        ignore xattrs and ACLs of files
-n|--no-inline
        disable inlining of small files to metadata blocks, this will decrease the metadata
        consumption and may help to convert a filesystem with low free space
-N|--nodesize <SIZE>
        set filesystem nodesize, the tree block size in which btrfs stores its metadata.
        The default value is 16KiB (16384) or the page size, whichever is bigger.
        Must be a multiple of the sectorsize, but not larger than 65536. See
        :doc:`mkfs.btrfs(8)<mkfs.btrfs>` for more details.
-r|--rollback
        rollback to the original ext2/3/4 filesystem if possible
-l|--label <LABEL>
        set filesystem label during conversion
-L|--copy-label
        use label from the converted filesystem
-O|--features <feature1>[,<feature2>...]
        A list of filesystem features enabled the at time of conversion. Not all features
        are supported by old kernels. To disable a feature, prefix it with *^*.
        Description of the features is in section *FILESYSTEM FEATURES* of
        :doc:`mkfs.btrfs(8)<mkfs.btrfs>`.

        To see all available features that btrfs-convert supports run:

        .. code-block:: bash

                btrfs-convert -O list-all

-p|--progress
        show progress of conversion (a heartbeat indicator and number of inodes
        processed), on by default

--no-progress
        disable progress and show only the main phases of conversion
--uuid <SPEC>
        set the FSID of the new filesystem based on 'SPEC':

        * *new* - (default) generate UUID for the FSID of btrfs
        * *copy* - copy UUID from the source filesystem
        * *UUID* - a conforming UUID value, the 36 byte string representation

EXIT STATUS
-----------

**btrfs-convert** will return 0 if no error happened.
If any problems happened, 1 will be returned.

SEE ALSO
--------

:doc:`mkfs.btrfs(8)<mkfs.btrfs>`
