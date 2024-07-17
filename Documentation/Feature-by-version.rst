Changes (feature/version)
=========================

Major features or significant feature enhancements by kernel version. For more
information look below.

The version states at which version a feature has been merged into the mainline
kernel. It does not tell anything about at which kernel version it is
considered mature enough for production use. For an estimation on stability of
features see :doc:`Status<Status>` page.

6.x
---

6.0 - send protocol v2
        Send protocol update that adds new commands and extends existing
        functionality to write large data chunks. Compressed (and encrypted)
        extents can be optionally emitted and transferred as-is without the need
        to re-compress (or re-encrypt) on the receiving side.

6.0 - sysfs exports commit stats
        The file :file:`/sys/fs/btrfs/FSID/commit_stats` shows number of commits and
        various time related statistics.

6.0 - sysfs exports chunk sizes
        Chunk size value can be read from
        :file:`/sys/fs/btrfs/FSID/allocation/PROFILE/chunk_size`.

6.0 - sysfs shows zoned mode among features
        The zoned mode has been supported since 5.10 and adding functionality.
        Now it's advertised among features.

6.0 - checksum implementation is logged at mount time
        When a filesystem is mounted the implementation backing the checksums
        is logged. The information is also accessible in
        :file:`/sys/fs/btrfs/FSID/checksum`.

6.1 - sysfs support to temporarily skip exact qgroup accounting
        Allow user override of qgroup accounting and make it temporarily out
        of date e.g. in case when there are several subvolumes deleted and the
        qgroup numbers need to be updated at some cost, an update after that
        can amortize the costs.

6.1 - scrub also repairs superblock
        An improvement to scrub in case the superblock is detected to be
        corrupted, the repair happens immediately. Previously it was delayed
        until the next transaction commit for performance reasons that would
        store an updated and correct copy eventually.

6.1 - block group tree
        An incompatible change that has to be enabled at mkfs time. Add a new
        b-tree item that stores information about block groups in a compact way
        that significantly improves mount time that's usually long due to
        fragmentation and scattered b-tree items tracking the individual block
        groups. Requires and also enables the free-space-tree and no-holes
        features.

6.1 - discard stats available in sysfs
        The directory :file:`/sys/fs/btrfs/FSID/discard` exports statistics and
        tunables related to discard.

6.1 - additional qgroup stats in sysfs
        The overall status of qgroups are exported in
        :file:`/sys/sys/fs/btrfs/FSID/qgroups/`.

6.1 - check that super block is unchanged at thaw time
        Do full check of super block once a filesystem is thawed. This namely
        happens when system resumes from suspend or hibernation. Accidental
        change by other operating systems will be detected.

6.2 - discard=async on by default
        Devices that support trim/discard will enable the asynchronous discard
        for the whole filesystem.

6.3 - discard=async settings tuned
        The default IOPS limit has changed from 100 to 1000 and writing value 0
        to :file:`/sys/fs/btrfs/FSID/discard/iops_limit` newly means to not do any
        throttling.

6.3 - block group allocation class heuristics
        Pack files by size (up to 128k, up to 8M, more) to avoid fragmentation
        in block groups, assuming that file size and life time is correlated,
        in particular this may help during balance. The stats about the number
        of used classes per block group type is exported in
        :file:`/sys/fs/btrfs/FSID/allocation/\*/size_classes`.

6.3 - in DEV_INFO ioctl export per-device FSID
        A seeding device could have a different FSID, available in sysfs and now
        available via DEV_INFO ioctl.

6.3 - send utimes cache, reduced stream size
        Utimes for directories are emitted into the send stream only when
        finalizing the directory, the cache also gains significant speedups (up
        to 10x).

6.7 - raid-stripe-tree
        New tree for logical mapping, allows some RAID modes for zoned mode.

6.7 - simplified quota accounting
        A simplified mode of qgroups accounting

6.7 - temporary fsid
        Mount of cloned devices is now possible, the filesystem will get a new
        randomly generated UUID on mount

6.8 - new mount API
        Use new mount API (https://lwn.net/Articles/753473/)

5.x
---

5.0 - swapfile
        With some limitations where COW design does not work well with the swap
        implementation (nodatacow file, no compression, cannot be snapshotted,
        not possible on multiple devices, ...), as this is the most restricted
        but working setup, we'll try to improve that in the future

5.0 - metadata uuid
        An optional incompat feature to assign a new filesystem UUID without
        overwriting all metadata blocks, stored only in superblock, unlike what
        :command:`btrfstune -u`

5.1 - FORGET_DEV ioctl
        Unregister devices previously added by the scan ioctl, same effect as
        if the kernel module is reloaded.

5.1 - ZSTD level
        Allow to set the ZSTD compression level via mount option, e.g. like
        *compress=zstd:9*. The levels match the default ZSTD compression
        levels. The default is 3, maximum is 15.

5.2 - pre-write checks
        Verify metadata blocks before submitting them to the devices. This can
        catch consistency problems or bitflips.

5.5 - more checksums
        New checksum algorithms: xxhash (64b), SHA256 (256b), BLAKE2b (256b).

5.5 - RAID1C34
        RAID1 with 3- and 4- copies (over all devices).

5.6 - async discard
        Mode of discard (*mount -o discard=async*) that merges freed extents to
        larger chunks and submits them for discard in a less intrusive way

5.6 - device info in sysfs
        More information about device state can be found in per-filesystem sysfs directory.

5.7 - reflink/clone works on inline files
        Inline files can be reflinked to the tail extent of other files

5.7 - faster balance cancel
        More cancellation points in balance that will shorten the time to stop
        processing once :command:`btrfs balance cancel` is called.

5.7 - *removed flag BTRFS_SUBVOL_CREATE_ASYNC*
        Remove support of flag BTRFS_SUBVOL_CREATE_ASYNC from subvolume creation ioctl.

5.7 - v2 of snapshot deletion ioctl
        New ioctl BTRFS_IOC_SNAP_DESTROY_V2, deletion by subvolume id is now possible.

5.9 - mount option *rescue*
        Unified mount option for actions that may help to access a damaged
        filesystem. Now supports: nologreplay, usebackuproot

5.9 - qgroups in sysfs
        The information about qgroup status and relations is exported in :file:`/sys/fs/UUID/qgroups`

5.9 - FS_INFO ioctl
        Export more information: checksum type, checksum size, generation, metadata_uuid

5.10 - exclusive ops in sysfs
        Export which filesystem exclusive operation is running (balance,
        resize, device add/delete/replace, ...)

5.11 - remove *inode_cache*
        Remove inode number caching feature (mount -o inode_cache)

5.11 - more rescue= modes
        Additional modes for mount option *rescue=*: ignorebadroots/ibadroots,
        ignoredatacsums/idatacsums. All are exported in
        :file:`/sys/fs/btrfs/features/supported_rescue_options`.

5.12 - zoned mode
        Support for zoned devices with special allocation/write mode to
        fixed-size zones. See :doc:`Zoned<Zoned-mode>`.

5.13 - supported_sectorsizes in sysfs
        List supported sector sizes in sysfs file :file:`/sys/fs/btrfs/features/supported_sectorsizes`.

5.14 - sysfs scrub bw limit
        Tunable bandwidth limit
        :file:`/sys/fs/btrfs/FSID/devinfo/DEVID/scrub_speed_max` for scrub (and
        device replace) for a given device.

5.14 - sysfs device stats
        The device stats can be also found in :file:`/sys/fs/btrfs/FSID/devinfo/DEVID/error_stats`.

5.14 - cancellable resize, device delete
        The filesystem resize and device delete operations can be cancelled by
        specifying *cancel* as the device name.

5.14 - property value reset
        Change how empty value is interpreted. New behaviour will delete the
        value and reset it to default. This affects *btrfs.compression* where
        value *no* sets NOCOMPRESS bit while empty value resets all compression
        settings (either compression or NOCOMPRESS bit).

5.15 - fsverity
        The fs-verity is a support layer that filesystems can hook into to
        support transparent integrity and authenticity protection of read-only
        files. https://www.kernel.org/doc/html/latest/filesystems/fsverity.html

5.15 - idmapped mount
        Support mount with UID/GID mapped according to another namespace.
        https://lwn.net/Articles/837566/

5.16 - ZNS in zoned
        Zoned namespaces. https://zonedstorage.io/docs/introduction/zns ,
        https://lwn.net/Articles/865988/

5.17 - send and relocation
        Send and relocation (balance, device remove, shrink, block group
        reclaim) can now work in parallel.

5.17 - device add vs balance
        It is possible to add a device with paused balance.

        .. note::
           Since kernel 5.17.7 and btrfs-progs 5.17.1

5.17 - *no warning with flushoncommit*
        Mounting with *-o flushoncommit* does not trigger the (harmless)
        warning at each transaction commit.

        .. note::
           Also backported to 5.15.27 and 5.16.13

5.18 - zoned and DUP metadata
        DUP metadata works with zoned mode.

5.18 - encoded data ioctl
        New ioctls to read and write pre-encoded data (i.e. no transformation
        and directly written as extents), now works for compressed data.

5.18 - *removed balance ioctl v1*
        The support for ioctl BTRFS_IOC_BALANCE has been removed, superseded by
        BTRFS_IOC_BALANCE_V2 long time ago.

5.18 - *cross-mount reflink works*
        The VFS limitation to reflink files on separate subvolume mounts of the
        same filesystem has been removed.

5.18 - syslog error messages with filesystem state
        Messages are printed with a one letter tag ("state: X") that denotes in
        which state the filesystem was at this point:

        * A - transaction aborted (permanent)
        * E - filesystem error (permanent)
        * M - remount in progress (transient)
        * R - device replace in progress (transient)
        * C - checksum checks disabled by mount option (rescue=ignoredatacsums)
        * L - log tree replay did not complete due to some error

5.18 - tree-checker verifies transaction id pre-write
        Metadata buffer to be written gets an extra check if the stored
        transaction number matches the current state of the filesystem.

5.19 - subpage support pages > 4KiB
        Metadata node size is supported regardless of the CPU page size
        (minimum size is 4KiB), data sector size is supported <= page size.
        Additionally subpage also supports RAID56.

5.19 - per-type background threshold for reclaim
        Add sysfs tunable for background reclaim threshold for all block group
        types (data, metadata, system).

5.19 - automatically repair device number mismatch
        Device information is stored in two places, the number in the super
        block and items in the device tree. When this is goes out of sync, e.g.
        by device removal short before unmount, the next mount could fail.
        The b-tree is an authoritative information an can be used to override
        the stale value in the superblock.

5.19 - defrag can convert inline files to regular ones
        The logic has been changed so that inline files are considered for
        defragmentation even if the mount option max_inline would prevent that.
        No defragmentation might happen but the inlined files are not skipped.

5.19 - explicit minimum zone size is 4MiB
        Set the minimum limit of zone on zoned devices to 4MiB. Real devices
        zones are much larger, this is for emulated devices.

5.19 - sysfs tunable for automatic block group reclaim
        Add possibility to set a threshold to automatically reclaim block groups
        also in non-zoned mode. By default completely empty block groups are
        reclaimed automatically but the threshold can be tuned in
        :file:`/sys/fs/btrfs/FSID/allocation/PROFILE/bg_reclaim_threshold`.

5.19 - tree-checker verifies metadata block ownership
        Additional check done by tree-checker to verify relationship between a
        tree block and it's tree root owner.

4.x
---

4.0 - store otime
        Save creation time (otime) for all new files and directories. For
        future use, current tool cannot read it directly.

4.2 - rootid ioctl accessible
        The INO_LOOKUP will return root id (id of the containing subvolume),
        unrestricted and to all users if the *treeid* is 0.

4.2 - dedupe possible on the same inode
        The EXTENT_SAME ioctl will accept the same inode as source and
        destination (ranges must not overlap).

4.3 - trim all free space
        Trim will be performed also on the space that's not allocated by the
        chunks, not only free space within the allocated chunks.

4.4 - balance filter updates
        Enhanced syntax and new balance filters:

        *  limit=min..max
        *  usage=min..max
        *  stripes=min..max

4.5 - free space tree
        Improved implementation of free space cache (aka v2), using b-trees.

        .. note::
           Default since btrfs-progs 5.15, Kernel 4.9 fixes endianness bugs on
           big-endian machines, x86* is ok

4.5 - balance filter updates
        Conversion to data/DUP profile possible through balance filters -- on single-device filesystem.

        .. note::
           mkfs.btrfs allows creating DUP on single device in the non-mixed mode since 4.4

4.6 - max_inline default
        The default value of max_inline changed to 2048.

4.6 - read features from control device
        The existing ioctl GET_SUPPORTED_FEATURES can be now used on the
        control device (:file:`/dev/btrfs-control`) and returns the supported features
        without any mounted filesystem.

4.7 - delete device by id
        Add new ioctl RM_DEV_V2, pass device to be deleted by its ID.

4.7 - more renameat2 modes
        Add support for RENAME_EXCHANGE and RENAME_WHITEOUT to *renameat2*
        syscall. This also means that *overlayfs* is now supported on top of
        btrfs.

4.7 - balance filter updates
        Conversion to data/DUP profile possible through balance filters -- on multiple-device filesystems.

        .. note::
           mkfs.btrfs allows creating DUP on multiple devices since 4.5.1

4.12 - RAID56: auto repair
        Scrub will attempt auto-repair (similar to raid1/raid10)

4.13 - statx
        Support for the enhanced statx syscall; file creation timestamp

4.13 - sysfs qgroups override
        qgroups: new sysfs control file to allow temporary quota override with CAP_SYS_RESOURCE

4.13 - *deprecated mount option alloc_start*
        That was a debugging helper, not used and not supposed to be used nowadays.

4.14 - ZSTD compression
        New compression algorithm ZSTD, supposedly better ratio/speed performance.

4.14 - improved degraded mount
        Allow degraded mount based on the chunk constraints, not device number
        constraints. E.g. when one device is missing but the remaining one holds
        all *single* chunks.

4.14 - *deprecated user transaction ioctl*
        BTRFS_IOC_TRANS_START and BTRFS_IOC_TRANS_END, no known users, tricky
        to use; scheduled to be removed in 4.17

4.14 - refine SSD optimizations
        The mount option *ssd* does not make any assumptions about block layout
        or management by the device anymore, leaving only the speedups based on
        low seek cost active.  This could avoid some corner cases leading to
        excessive fragmentation.
        https://git.kernel.org/linus/583b723151794e2ff1691f1510b4e43710293875
        The story so far.

4.15 - overlayfs
        Overlayfs can now use btrfs as the lower filesystem.

4.15 - *ref-verify*
        Debugging functionality to verify extent references. New mount option
        *ref-verify*, must be built with CONFIG_BTRFS_FS_REF_VERIFY.

4.15 - ZLIB level
        Allow to set the ZLIB compression level via mount option, e.g. like
        *compress=zlib:9*. The levels match the default ZLIB compression
        levels. The default is 3.

4.15 - v2 of LOGICAL_INO ioctl
        An enhanced version of ioctl that can translate logical extent offset
        to inode numbers, "who owns this block". For certain use cases the V1
        performs bad and this is addressed by V2.
        See for more https://git.kernel.org/linus/d24a67b2d997c860a42516076f3315c2ad2d2884 .

4.15 - compression heuristics
        Apply a few heuristics to the data before they're compressed to decide
        if it's likely to gain any space savings. The methods: frequency
        sampling, repeated pattern detection, Shannon entropy calculation.

4.16 - fallocate: zero range
        Mode of the *fallocate* syscall to zero file range.

4.17 - *removed user transaction ioctl*
        Deprecated in 4.14, see above.

4.17 - *rmdir* on subvolumes
        Allow *rmdir* to delete an empty subvolume.

4.18 - XFLAGS ioctl
        Add support for ioctl FS_IOC_FSSETXATTR/FS_IOC_FSGETXATTR, successor of
        FS_IOC_SETFLAGS/FS_IOC_GETFLAGS ioctl. Currently supports: APPEND,
        IMMUTABLE, NOATIME, NODUMP, SYNC. Note that the naming is very
        confusing, though it's named *xattr*, it does not mean the extended
        attributes. It should be referenced as extended inode flags or
        *xflags*.

4.18 - EXTENT_SAME ioctl / 16MiB chunks
        The range for out-of-band deduplication implemented by the EXTENT_SAME
        ioctl will split the range into 16MiB chunks. Up to now this was the
        overall limit and effectively only the first 16MiB was deduplicated.

4.18 - GET_SUBVOL_INFO ioctl
        New ioctl to read subvolume information (id, directory name,
        generation, flags, UUIDs, time). This does not require root
        permissions, only the regular access to to the subvolume.

4.18 - GET_SUBVOL_ROOTREF ioctl
        New ioctl to enumerate subvolume references of a given subvolume. This
        does not require root permissions, only the regular access to to the
        subvolume.

4.18 - INO_LOOKUP_USER ioctl
        New ioctl to lookup path by inode number. This does not require root
        permissions, only the regular access to to the subvolume, unlike the
        INO_LOOKUP ioctl.

4.19 - defrag ro/rw
        Allow to run defrag on files that are normally accessible for
        read-write, but are currently opened in read-only mode.

3.x
---

3.0 - scrub
        Read all data and verify checksums, repair if possible.

3.2 - auto raid repair
        Automatic repair of broken data from a good copy

3.2 - root backups
        Save a few previous versions of the most important tree roots at commit time, used by *-o recovery*

3.3 - integrity checker
        Optional infrastructure to verify integrity of written metadata blocks

3.3 - backref walking
        Groundwork to allow tracking owner of blocks, used via *inspect-internal*

3.3 - restriper
        RAID profiles can be changed on-line, balance filters

3.4 - big metadata blocks
        Support for metadata blocks larger than page size

        .. note::
           Default nodesize is 16KiB since btrfs-progs 3.12

3.4 - error handling
        Generic infrastructure for graceful error handling (EIO)

3.5 - device statistics
        Persistent statistics about device errors

3.5 - fsync speedup
        Noticeable improvements in fsync() implementation

3.6 - qgroups
        Subvolume-aware quotas

3.6 - send/receive
        Ability to transfer one filesystem via a data stream (full or
        incremental) and apply the changes on a remote filesystem.

3.7 - extrefs
        Hardlink count limit is lifted to 65536.

        .. note::
           Default since btrfs-progs 3.12

3.7 - hole punching
        Implement the FALLOC_FL_PUNCH_HOLE mode of *fallocate*.

3.8 - device replace
        Efficient replacement of existing device (add/remove in one go).

3.9 - raid 5/6 *(incomplete)*
        Basic support for RAID5/6 profiles, no crash resiliency, replace and
        scrub support.

3.9 - snapshot-aware defrag
        Defrag does not break links between shared extents (snapshots,
        reflinked files).

        .. note::
           Disabled since 3.14 (and backported to some stable kernel versions)
           due to problems. Has been completely removed in 5.6.

3.9 - lightweight send
        A mode of *send* that does not add the actual file data to the stream.

3.9 - on-line label set/get
        Label editable on mounted filesystems.

3.10 - skinny metadata
        Reduced metadata size (format change) of extents.

       .. note::
          Default since btrfs-progs 3.18

3.10 - qgroup rescan
        Sync qgroups with existing filesystem data.

3.12 - UUID tree
        A map of subvolume/UUID that vastly speeds up send/receive.

3.12 - out-of-bound deduplication
        Support for deduplicating extents on a given set of files.

3.14 - no-holes
        No extent representation for file holes (format change), may reduce
        overall metadata consumption

3.14 - feature bits in sysfs
        :file:`/sys/fs/btrfs` exports various bits about filesystem
        capabilities and feature support

3.16 - O_TMPFILE
        Mode of open() to safely create a temporary file

3.16 - search ioctl v2
        The extended SEARCH_TREE ioctl able to get more than a 4k data

3.18 - auto block group reclaim
        Automatically remove block groups (aka. chunks) that become completely empty.

3.19 - RAID56: scrub, replace
        Scrub and device replace works on RAID56 filesystems.
