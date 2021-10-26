btrfs-image(8)
==============

SYNOPSIS
--------
**btrfs-image** [options] <source> <target>

DESCRIPTION
-----------

**btrfs-image** is used to create an image of a btrfs filesystem.
All data will be zeroed, but metadata and the like is preserved.
Mainly used for debugging purposes.

In the dump mode, source is the btrfs device/file and target is the output
file (use *-* for stdout).

In the restore mode (option *-r*), source is the dumped image and target is the btrfs device/file.

OPTIONS
-------

-r
        Restore metadump image. By default, this fixes super's chunk tree, by
        using 1 stripe pointing to primary device, so that file system can be
        restored by running tree log reply if possible. To restore without
        changing number of stripes in chunk tree check *-o* option.

-c <value>
        Compression level (0 ~ 9).

-t <value>
        Number of threads (1 ~ 32) to be used to process the image dump or restore.

-o
        Use the old restore method, this does not fixup the chunk tree so the restored
        file system will not be able to be mounted.

-s
        Sanitize the file names when generating the image. One -s means just
        generate random garbage, which means that the directory indexes won't match up
        since the hashes won't match with the garbage filenames. Using -ss will
        calculate a collision for the filename so that the hashes match, and if it
        can't calculate a collision then it will just generate garbage.  The collision
        calculator is very time and CPU intensive so only use it if you are having
        problems with your file system tree and need to have it mostly working.

-w
        Walk all the trees manually and copy any blocks that are referenced. Use this
        option if your extent tree is corrupted to make sure that all of the metadata is
        captured.

-m
        Restore for multiple devices, more than 1 device should be provided.

EXIT STATUS
-----------

**btrfs-image** will return 0 if no error happened.
If any problems happened, 1 will be returned.

SEE ALSO
--------

``mkfs.btrfs(8)``
