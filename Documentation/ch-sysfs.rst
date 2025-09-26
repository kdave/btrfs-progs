Btrfs has a sysfs interface to provide extra knobs.

The top level path is :file:`/sys/fs/btrfs/`, and the main directory layout is the following:

=============================  ===================================  ========
Relative Path                  Description                          Version
=============================  ===================================  ========
features/                      All supported features               3.14
<UUID>/                        Mounted fs UUID                      3.14
<UUID>/allocation/             Space allocation info                3.14
<UUID>/bdi/                    Backing device info (writeback)      5.9
<UUID>/devices/<DEVID>/        Symlink to each block device sysfs   5.6
<UUID>/devinfo/<DEVID>/        Btrfs specific info for each device  5.6
<UUID>/discard/                Discard stats and tunables           6.1
<UUID>/features/               Features of the filesystem           3.14
<UUID>/qgroups/                Global qgroup info                   5.9
<UUID>/qgroups/<LEVEL>_<ID>/   Info for each qgroup                 5.9
=============================  ===================================  ========

For :file:`/sys/fs/btrfs/features/` directory, each file means a supported feature
of the current kernel. Most files have value 0. Otherwise it depends on the file,
value *1* typically means the feature can be turned on a mounted filesystem.

For :file:`/sys/fs/btrfs/<UUID>/features/` directory, each file means an enabled
feature on the mounted filesystem.

The features share the same name in section
:ref:`FILESYSTEM FEATURES<man-btrfs5-filesystem-features>`.

UUID
^^^^

Files in :file:`/sys/fs/btrfs/<UUID>/` directory are:

bg_reclaim_threshold
        (RW, since: 5.19)

        Used space percentage of total device space to start auto block group claim.
        Mostly for zoned devices.

checksum
        (RO, since: 5.5)

        The checksum used for the mounted filesystem.
        This includes both the checksum type (see section
        :ref:`CHECKSUM ALGORITHMS<man-btrfs5-checksum-algorithms>`)
        and the implemented driver (mostly shows if it's hardware accelerated).

clone_alignment
        (RO, since: 3.16)

        The bytes alignment for *clone* and *dedupe* ioctls.

commit_stats
        (RW, since: 6.0)

        The performance statistics for btrfs transaction commit since the first
        mount. Mostly for debugging purposes.

        Writing into this file will reset the maximum commit duration
        (*max_commit_ms*) to 0. The file looks like:

        .. code-block:: none

                commits 70649
                last_commit_ms 2
                max_commit_ms 131
                total_commit_ms 170840

        * *commits* - number of transaction commits since the first mount
        * *last_commit_ms* - duration in milliseconds of the last commit
        * *max_commit_ms* - maximum time a transaction commit took since first mount or last reset
        * *total_commit_ms* - sum of all transaction commit times

exclusive_operation
        (RO, since: 5.10)

        Shows the running exclusive operation.
        Check section
        :ref:`FILESYSTEM EXCLUSIVE OPERATIONS<man-btrfs5-filesystem-exclusive-operations>`
        for details.

generation
        (RO, since: 5.11)

        Show the generation of the mounted filesystem.

label
        (RW, since: 3.14)

        Show the current label of the mounted filesystem.

metadata_uuid
        (RO, since: 5.0)

        Shows the metadata UUID of the mounted filesystem.
        Check `metadata_uuid` feature for more details.

nodesize
        (RO, since: 3.14)

        Show the nodesize of the mounted filesystem.

quota_override
        (RW, since: 4.13)

        Shows the current quota override status.
        0 means no quota override.
        1 means quota override, quota can ignore the existing limit settings.

read_policy
        (RW, since: 5.11)

        Shows the current balance policy for reads.
        Currently only ``pid`` (balance using the process id (pid) value) is
        supported. More balancing policies are available in experimental
        build, namely round-robin.

sectorsize
        (RO, since: 3.14)

        Shows the sectorsize of the mounted filesystem.

temp_fsid
        (RO, since 6.7)

        Indicate that this filesystem got assigned a temporary FSID at mount time,
        making possible to mount devices with the same FSID.

UUID/allocations
^^^^^^^^^^^^^^^^

Files and directories in :file:`/sys/fs/btrfs/<UUID>/allocations` directory are:

global_rsv_reserved
        (RO, since: 3.14)

        The used bytes of the global reservation.

global_rsv_size
        (RO, since: 3.14)

        The total size of the global reservation.

`data/`, `metadata/` and `system/` directories
        (RO, since: 5.14)

        Space info accounting for the 3 block group types.

UUID/allocations/{data,metadata,system}
"""""""""""""""""""""""""""""""""""""""

Files in :file:`/sys/fs/btrfs/<UUID>/allocations/{data,metadata,system}` directory are:

bg_reclaim_threshold
        (RW, since: 5.19)

        Reclaimable space percentage of block group's size (excluding
        permanently unusable space) to reclaim the block group.
        Can be used on regular or zoned devices.

bytes_*
        (RO)

        Values of the corresponding data structures for the given block group
        type and profile that are used internally and may change rapidly depending
        on the load.

        Complete list: bytes_may_use, bytes_pinned, bytes_readonly,
        bytes_reserved, bytes_used, bytes_zone_unusable

chunk_size
        (RW, since: 6.0)

        Shows the chunk size. Can be changed for data and metadata (independently)
        and cannot be set for system block group type.
        Cannot be set for zoned devices as it depends on the fixed device zone size.
        Upper bound is 10% of the filesystem size, the value must be multiple of 256MiB
        and greater than 0.

size_classes
        (RO, since: 6.3)

        Numbers of block groups of a given classes based on heuristics that
        measure extent length, age and fragmentation.

        .. code-block:: none

                none 136
                small 374
                medium 282
                large 93

UUID/bdi
^^^^^^^^

Symlink to the sysfs directory of the backing device info (BDI), which is
related to writeback process and infrastructure.

UUID/devices
^^^^^^^^^^^^

Files in :file:`/sys/fs/btrfs/<UUID>/devices` directory are symlinks named
after device nodes (e.g. sda, dm-0) and pointing to their sysfs directory.

UUID/devinfo
^^^^^^^^^^^^

The directory contains subdirectories named after device ids (numeric values). Each
subdirectory has information about the device of the given *devid*.

UUID/devinfo/DEVID
""""""""""""""""""

Files in :file:`/sys/fs/btrfs/<UUID>/devinfo/<DEVID>` directory are:

error_stats:
        (RO, since: 5.14)

        Shows device stats of this device, same as :command:`btrfs device stats` (:doc:`btrfs-device`).

        .. code-block:: none

                write_errs 0
                read_errs 0
                flush_errs 0
                corruption_errs 0
                generation_errs 0

fsid:
        (RO, since: 5.17)

        Shows the fsid which the device belongs to.
        It can be different than the ``UUID`` if it's a seed device.

in_fs_metadata
        (RO, since: 5.6)

        Shows whether we have found the device.
        Should always be 1, as if this turns to 0, the :file:`DEVID` directory
        would get removed automatically.

missing
        (RO, since: 5.6)

        Shows whether the device is considered missing by the kernel module.

replace_target
        (RO, since: 5.6)

        Shows whether the device is the replace target.
        If no device replace is running, this value is 0.

scrub_speed_max
        (RW, since: 5.14)

        Shows the scrub speed limit for this device. The unit is Bytes/s.
        0 means no limit. The value can be set but is not persistent.

writeable
        (RO, since: 5.6)

        Show if the device is writeable.

UUID/qgroups
^^^^^^^^^^^^

Files in :file:`/sys/fs/btrfs/<UUID>/qgroups/` directory are:

enabled
        (RO, since: 6.1)

        Shows if qgroup is enabled.
        Also, if qgroup is disabled, the :file:`qgroups` directory will
        be removed automatically.

inconsistent
        (RO, since: 6.1)

        Shows if the qgroup numbers are inconsistent.
        If 1, it's recommended to do a qgroup rescan.

drop_subtree_threshold
        (RW, since: 6.1)

        Shows the subtree drop threshold to automatically mark qgroup inconsistent.

        When dropping large subvolumes with qgroup enabled, there would be a huge
        load for qgroup accounting.
        If we have a subtree whose level is larger than or equal to this value,
        we will not trigger qgroup account at all, but mark qgroup inconsistent to
        avoid the huge workload.

        Default value is 3, which means that trees of low height will be accounted
        properly as this is sufficiently fast. The value was 8 until 6.13 where
        no subtree drop can trigger qgroup rescan making it less useful.

        Lower value can reduce qgroup workload, at the cost of extra qgroup rescan
        to re-calculate the numbers.

UUID/qgroups/LEVEL_ID
"""""""""""""""""""""

Files in each :file:`/sys/fs/btrfs/<UUID>/qgroups/<LEVEL>_<ID>/` directory are:

exclusive
        (RO, since: 5.9)

        Shows the exclusively owned bytes of the qgroup.

limit_flags
        (RO, since: 5.9)

        Shows the numeric value of the limit flags.
        If 0, means no limit implied.

max_exclusive
        (RO, since: 5.9)

        Shows the limits on exclusively owned bytes.

max_referenced
        (RO, since: 5.9)

        Shows the limits on referenced bytes.

referenced
        (RO, since: 5.9)

        Shows the referenced bytes of the qgroup.

rsv_data
        (RO, since: 5.9)

        Shows the reserved bytes for data.

rsv_meta_pertrans
        (RO, since: 5.9)

        Shows the reserved bytes for per transaction metadata.

rsv_meta_prealloc
        (RO, since: 5.9)

        Shows the reserved bytes for preallocated metadata.

UUID/discard
^^^^^^^^^^^^

Files in :file:`/sys/fs/btrfs/<UUID>/discard/` directory are:

discardable_bytes
        (RO, since: 6.1)

        Shows amount of bytes that can be discarded in the async discard and
        nodiscard mode.

discardable_extents
        (RO, since: 6.1)

        Shows number of extents to be discarded in the async discard and
        nodiscard mode.

discard_bitmap_bytes
        (RO, since: 6.1)

        Shows amount of discarded bytes from data tracked as bitmaps.

discard_extent_bytes
        (RO, since: 6.1)

        Shows amount of discarded extents from data tracked as bitmaps.

discard_bytes_saved
        (RO, since: 6.1)

        Shows the amount of bytes that were reallocated without being discarded.

kbps_limit
        (RW, since: 6.1)

        Tunable limit of kilobytes per second issued as discard IO in the async
        discard mode.

iops_limit
        (RW, since: 6.1)

        Tunable limit of number of discard IO operations to be issued in the
        async discard mode.

max_discard_size
        (RW, since: 6.1)

        Tunable limit for size of one IO discard request.
