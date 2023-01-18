Btrfs has a sysfs interface to provide extra knobs.

The top level path is `/sys/fs/btrfs/`, and the main directory layout is the following:

=============================  ===================================  ========
Relative Path                  Description                          Version
=============================  ===================================  ========
features/                      All supported features               3.14+
<UUID>/                        Mounted fs UUID                      3.14+
<UUID>/allocation/             Space allocation info                3.14+
<UUID>/features/               Features of the filesystem           3.14+
<UUID>/devices/<DEVID>/        Symlink to each block device sysfs   5.6+
<UUID>/devinfo/<DEVID>/        Btrfs specific info for each device  5.6+
<UUID>/qgroups/                Global qgroup info                   5.9+
<UUID>/qgroups/<LEVEL>_<ID>/   Info for each qgroup                 5.9+
=============================  ===================================  ========

For `/sys/fs/btrfs/features/` directory, each file means a supported feature
for the current kernel.

For `/sys/fs/btrfs/<UUID>/features/` directory, each file means an enabled
feature for the mounted filesystem.

The features shares the same name in section *FILESYSTEM FEATURES*.


Files in `/sys/fs/btrfs/<UUID>/` directory are:

bg_reclaim_threshold
        (RW, since: 5.19)

        Used space percentage of total device space to start auto block group claim.
        Mostly for zoned devices.

checksum
        (RO, since: 5.5)

        The checksum used for the mounted filesystem.
        This includes both the checksum type (see section *CHECKSUM ALGORITHMS*)
        and the implemented driver (mostly shows if it's hardware accelerated).

clone_alignment
        (RO, since: 3.16)

        The bytes alignment for *clone* and *dedupe* ioctls.

commit_stats
        (RW, since: 6.0)

        The performance statistics for btrfs transaction commit.
        Mostly for debug purposes.

        Writing into this file will reset the maximum commit duration to
        the input value.

exclusive_operation
        (RO, since: 5.10)

        Shows the running exclusive operation.
        Check section *FILESYSTEM EXCLUSIVE OPERATIONS* for details.

generation
        (RO, since: 5.11)

        Show the generation of the mounted filesystem.

label
        (RW, since: 3.14)

        Show the current label of the mounted filesystem.

metadata_uuid
        (RO, since: 5.0)

        Shows the metadata uuid of the mounted filesystem.
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
        Currently only "pid" (balance using pid value) is supported.

sectorsize
        (RO, since: 3.14)

        Shows the sectorsize of the mounted filesystem.


Files and directories in `/sys/fs/btrfs/<UUID>/allocations` directory are:

global_rsv_reserved
        (RO, since: 3.14)

        The used bytes of the global reservation.

global_rsv_size
        (RO, since: 3.14)

        The total size of the global reservation.

`data/`, `metadata/` and `system/` directories
        (RO, since: 5.14)

        Space info accounting for the 3 chunk types.
        Mostly for debug purposes.

Files in `/sys/fs/btrfs/<UUID>/allocations/{data,metadata,system}` directory are:

bg_reclaim_threshold
        (RW, since: 5.19)

        Reclaimable space percentage of block group's size (excluding
        permanently unusable space) to reclaim the block group.
        Can be used on regular or zoned devices.

chunk_size
        (RW, since: 6.0)

        Shows the chunk size. Can be changed for data and metadata.
        Cannot be set for zoned devices.

Files in `/sys/fs/btrfs/<UUID>/devinfo/<DEVID>` directory are:

error_stats:
        (RO, since: 5.14)

        Shows all the history error numbers of the device.

fsid:
        (RO, since: 5.17)

        Shows the fsid which the device belongs to.
        It can be different than the `<UUID>` if it's a seed device.

in_fs_metadata
        (RO, since: 5.6)

        Shows whether we have found the device.
        Should always be 1, as if this turns to 0, the `<DEVID>` directory
        would get removed automatically.

missing
        (RO, since: 5.6)

        Shows whether the device is missing.

replace_target
        (RO, since: 5.6)

        Shows whether the device is the replace target.
        If no dev-replace is running, this value should be 0.

scrub_speed_max
        (RW, since: 5.14)

        Shows the scrub speed limit for this device. The unit is Bytes/s.
        0 means no limit.

writeable
        (RO, since: 5.6)

        Show if the device is writeable.

Files in `/sys/fs/btrfs/<UUID>/qgroups/` directory are:

enabled
        (RO, since: 6.1)

        Shows if qgroup is enabled.
        Also, if qgroup is disabled, the `qgroups` directory would
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

        Default value is 8, where no subtree drop can trigger qgroup.

        Lower value can reduce qgroup workload, at the cost of extra qgroup rescan
        to re-calculate the numbers.

Files in `/sys/fs/btrfs/<UUID>/<LEVEL>_<ID>/` directory are:

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
