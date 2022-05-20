maximum file name length
        255

maximum symlink target length
        depends on the *nodesize* value, for 4KiB it's 3949 bytes, for larger nodesize
        it's 4095 due to the system limit PATH_MAX

        The symlink target may not be a valid path, ie. the path name components
        can exceed the limits (NAME_MAX), there's no content validation at ``symlink(3)``
        creation.

maximum number of inodes
        2\ :sup:`64` but depends on the available metadata space as the inodes are created
        dynamically

inode numbers
        minimum number: 256 (for subvolumes), regular files and directories: 257

maximum file length
        inherent limit of btrfs is 2\ :sup:`64` (16 EiB) but the linux VFS limit is 2\ :sup:`63` (8 EiB)

maximum number of subvolumes
        the subvolume ids can go up to 2\ :sup:`64` but the number of actual subvolumes
        depends on the available metadata space, the space consumed by all subvolume
        metadata includes bookkeeping of shared extents can be large (MiB, GiB)

maximum number of hardlinks of a file in a directory
        65536 when the *extref* feature is turned on during mkfs (default), roughly
        100 otherwise

minimum filesystem size
        the minimal size of each device depends on the *mixed-bg* feature, without that
        (the default) it's about 109MiB, with mixed-bg it's is 16MiB
