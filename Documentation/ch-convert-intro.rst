The **btrfs-convert** tool can be used to convert existing source filesystem
image to a btrfs filesystem in-place.  The original filesystem image is
accessible in subvolume named like *ext2_saved* as file *image*.

Supported filesystems:

* ext2, ext3, ext4 -- original feature, always built in

* reiserfs -- since version 4.13, optionally built, requires libreiserfscore 3.6.27

* ntfs -- external tool https://github.com/maharmstone/ntfs2btrfs

The list of supported source filesystem by a given binary is listed at the end
of help (option *--help*).

.. warning::
   If you are going to perform rollback to the original filesystem, you
   should not execute **btrfs balance** command on the converted filesystem. This
   will change the extent layout and make **btrfs-convert** unable to rollback.

The conversion utilizes free space of the original filesystem. The exact
estimate of the required space cannot be foretold. The final btrfs metadata
might occupy several gigabytes on a hundreds-gigabyte filesystem.

If the ability to rollback is no longer important, the it is recommended to
perform a few more steps to transition the btrfs filesystem to a more compact
layout. This is because the conversion inherits the original data blocks'
fragmentation, and also because the metadata blocks are bound to the original
free space layout.

Due to different constraints, it is only possible to convert filesystems that
have a supported data block size (ie. the same that would be valid for
**mkfs.btrfs**). This is typically the system page size (4KiB on x86_64
machines).

**BEFORE YOU START**

The source filesystem must be clean, eg. no journal to replay or no repairs
needed. The respective **fsck** utility must be run on the source filessytem prior
to conversion. Please refer to the manual pages in case you encounter problems.

For ext2/3/4:

.. code-block:: bash

    # e2fsck -fvy /dev/sdx

For reiserfs:

.. code-block:: bash

    # reiserfsck -fy /dev/sdx

Skipping that step could lead to incorrect results on the target filesystem,
but it may work.

**REMOVE THE ORIGINAL FILESYSTEM METADATA**

By removing the subvolume named like *ext2_saved* or *reiserfs_saved*, all
metadata of the original filesystem will be removed:

.. code-block:: bash

   # btrfs subvolume delete /mnt/ext2_saved

At this point it is not possible to do a rollback. The filesystem is usable but
may be impacted by the fragmentation inherited from the original filesystem.

**MAKE FILE DATA MORE CONTIGUOUS**

An optional but recommended step is to run defragmentation on the entire
filesystem. This will attempt to make file extents more contiguous.

.. code-block:: bash

   # btrfs filesystem defrag -v -r -f -t 32M /mnt/btrfs

Verbose recursive defragmentation (*-v*, *-r*), flush data per-file (*-f*) with
target extent size 32MiB (*-t*).

**ATTEMPT TO MAKE BTRFS METADATA MORE COMPACT**

Optional but recommended step.

The metadata block groups after conversion may be smaller than the default size
(256MiB or 1GiB). Running a balance will attempt to merge the block groups.
This depends on the free space layout (and fragmentation) and may fail due to
lack of enough work space. This is a soft error leaving the filesystem usable
but the block group layout may remain unchanged.

Note that balance operation takes a lot of time, please see also
:doc:`btrfs-balance(8)<btrfs-balance>`.

.. code-block:: bash

   # btrfs balance start -m /mnt/btrfs

