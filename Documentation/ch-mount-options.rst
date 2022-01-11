This section describes mount options specific to BTRFS.  For the generic mount
options please refer to ``mount(8)`` manpage. The options are sorted alphabetically
(discarding the *no* prefix).

.. note::
        Most mount options apply to the whole filesystem and only options in the
        first mounted subvolume will take effect. This is due to lack of implementation
        and may change in the future. This means that (for example) you can't set
        per-subvolume *nodatacow*, *nodatasum*, or *compress* using mount options. This
        should eventually be fixed, but it has proved to be difficult to implement
        correctly within the Linux VFS framework.

Mount options are processed in order, only the last occurrence of an option
takes effect and may disable other options due to constraints (see eg.
*nodatacow* and *compress*). The output of **mount** command shows which options
have been applied.

acl, noacl
        (default: on)

        Enable/disable support for Posix Access Control Lists (ACLs).  See the
        ``acl(5)`` manual page for more information about ACLs.

        The support for ACL is build-time configurable (BTRFS_FS_POSIX_ACL) and
        mount fails if *acl* is requested but the feature is not compiled in.

autodefrag, noautodefrag
        (since: 3.0, default: off)

        Enable automatic file defragmentation.
        When enabled, small random writes into files (in a range of tens of kilobytes,
        currently it's 64KiB) are detected and queued up for the defragmentation process.
        Not well suited for large database workloads.

        The read latency may increase due to reading the adjacent blocks that make up the
        range for defragmentation, successive write will merge the blocks in the new
        location.

        .. warning::
                Defragmenting with Linux kernel versions < 3.9 or ≥ 3.14-rc2 as
                well as with Linux stable kernel versions ≥ 3.10.31, ≥ 3.12.12 or
                ≥ 3.13.4 will break up the reflinks of COW data (for example files
                copied with **cp --reflink**, snapshots or de-duplicated data).
                This may cause considerable increase of space usage depending on the
                broken up reflinks.

barrier, nobarrier
        (default: on)

        Ensure that all IO write operations make it through the device cache and are stored
        permanently when the filesystem is at its consistency checkpoint. This
        typically means that a flush command is sent to the device that will
        synchronize all pending data and ordinary metadata blocks, then writes the
        superblock and issues another flush.

        The write flushes incur a slight hit and also prevent the IO block
        scheduler to reorder requests in a more effective way. Disabling barriers gets
        rid of that penalty but will most certainly lead to a corrupted filesystem in
        case of a crash or power loss. The ordinary metadata blocks could be yet
        unwritten at the time the new superblock is stored permanently, expecting that
        the block pointers to metadata were stored permanently before.

        On a device with a volatile battery-backed write-back cache, the *nobarrier*
        option will not lead to filesystem corruption as the pending blocks are
        supposed to make it to the permanent storage.

check_int, check_int_data, check_int_print_mask=<value>
        (since: 3.0, default: off)

        These debugging options control the behavior of the integrity checking
        module (the BTRFS_FS_CHECK_INTEGRITY config option required). The main goal is
        to verify that all blocks from a given transaction period are properly linked.

        *check_int* enables the integrity checker module, which examines all
        block write requests to ensure on-disk consistency, at a large
        memory and CPU cost.

        *check_int_data* includes extent data in the integrity checks, and
        implies the *check_int* option.

        *check_int_print_mask* takes a bitmask of BTRFSIC_PRINT_MASK_* values
        as defined in *fs/btrfs/check-integrity.c*, to control the integrity
        checker module behavior.

        See comments at the top of *fs/btrfs/check-integrity.c*
        for more information.

clear_cache
        Force clearing and rebuilding of the disk space cache if something
        has gone wrong. See also: *space_cache*.

commit=<seconds>
        (since: 3.12, default: 30)

        Set the interval of periodic transaction commit when data are synchronized
        to permanent storage. Higher interval values lead to larger amount of unwritten
        data, which has obvious consequences when the system crashes.
        The upper bound is not forced, but a warning is printed if it's more than 300
        seconds (5 minutes). Use with care.

compress, compress=<type[:level]>, compress-force, compress-force=<type[:level]>
        (default: off, level support since: 5.1)

        Control BTRFS file data compression.  Type may be specified as *zlib*,
        *lzo*, *zstd* or *no* (for no compression, used for remounting).  If no type
        is specified, *zlib* is used.  If *compress-force* is specified,
        then compression will always be attempted, but the data may end up uncompressed
        if the compression would make them larger.

        Both *zlib* and *zstd* (since version 5.1) expose the compression level as a
        tunable knob with higher levels trading speed and memory (*zstd*) for higher
        compression ratios. This can be set by appending a colon and the desired level.
        Zlib accepts the range [1, 9] and zstd accepts [1, 15]. If no level is set,
        both currently use a default level of 3. The value 0 is an alias for the
        default level.

        Otherwise some simple heuristics are applied to detect an incompressible file.
        If the first blocks written to a file are not compressible, the whole file is
        permanently marked to skip compression. As this is too simple, the
        *compress-force* is a workaround that will compress most of the files at the
        cost of some wasted CPU cycles on failed attempts.
        Since kernel 4.15, a set of heuristic algorithms have been improved by using
        frequency sampling, repeated pattern detection and Shannon entropy calculation
        to avoid that.

        .. note::
                If compression is enabled, *nodatacow* and *nodatasum* are disabled.

datacow, nodatacow
        (default: on)

        Enable data copy-on-write for newly created files.
        *Nodatacow* implies *nodatasum*, and disables *compression*. All files created
        under *nodatacow* are also set the NOCOW file attribute (see ``chattr(1)``).

        .. note::
                If *nodatacow* or *nodatasum* are enabled, compression is disabled.

        Updates in-place improve performance for workloads that do frequent overwrites,
        at the cost of potential partial writes, in case the write is interrupted
        (system crash, device failure).

datasum, nodatasum
        (default: on)

        Enable data checksumming for newly created files.
        *Datasum* implies *datacow*, ie. the normal mode of operation. All files created
        under *nodatasum* inherit the "no checksums" property, however there's no
        corresponding file attribute (see ``chattr(1)``).

        .. note::
                If *nodatacow* or *nodatasum* are enabled, compression is disabled.

        There is a slight performance gain when checksums are turned off, the
        corresponding metadata blocks holding the checksums do not need to updated.
        The cost of checksumming of the blocks in memory is much lower than the IO,
        modern CPUs feature hardware support of the checksumming algorithm.

degraded
        (default: off)

        Allow mounts with less devices than the RAID profile constraints
        require.  A read-write mount (or remount) may fail when there are too many devices
        missing, for example if a stripe member is completely missing from RAID0.

        Since 4.14, the constraint checks have been improved and are verified on the
        chunk level, not an the device level. This allows degraded mounts of
        filesystems with mixed RAID profiles for data and metadata, even if the
        device number constraints would not be satisfied for some of the profiles.

        Example: metadata -- raid1, data -- single, devices -- /dev/sda, /dev/sdb

        Suppose the data are completely stored on *sda*, then missing *sdb* will not
        prevent the mount, even if 1 missing device would normally prevent (any)
        *single* profile to mount. In case some of the data chunks are stored on *sdb*,
        then the constraint of single/data is not satisfied and the filesystem
        cannot be mounted.

device=<devicepath>
        Specify a path to a device that will be scanned for BTRFS filesystem during
        mount. This is usually done automatically by a device manager (like udev) or
        using the **btrfs device scan** command (eg. run from the initial ramdisk). In
        cases where this is not possible the *device* mount option can help.

        .. note::
                Booting eg. a RAID1 system may fail even if all filesystem's *device*
                paths are provided as the actual device nodes may not be discovered by the
                system at that point.

discard, discard=sync, discard=async, nodiscard
        (default: off, async support since: 5.6)

        Enable discarding of freed file blocks.  This is useful for SSD devices, thinly
        provisioned LUNs, or virtual machine images; however, every storage layer must
        support discard for it to work.

        In the synchronous mode (*sync* or without option value), lack of asynchronous
        queued TRIM on the backing device TRIM can severely degrade performance,
        because a synchronous TRIM operation will be attempted instead. Queued TRIM
        requires newer than SATA revision 3.1 chipsets and devices.

        The asynchronous mode (*async*) gathers extents in larger chunks before sending
        them to the devices for TRIM. The overhead and performance impact should be
        negligible compared to the previous mode and it's supposed to be the preferred
        mode if needed.

        If it is not necessary to immediately discard freed blocks, then the ``fstrim``
        tool can be used to discard all free blocks in a batch. Scheduling a TRIM
        during a period of low system activity will prevent latent interference with
        the performance of other operations. Also, a device may ignore the TRIM command
        if the range is too small, so running a batch discard has a greater probability
        of actually discarding the blocks.

enospc_debug, noenospc_debug
        (default: off)

        Enable verbose output for some ENOSPC conditions. It's safe to use but can
        be noisy if the system reaches near-full state.

fatal_errors=<action>
        (since: 3.4, default: bug)

        Action to take when encountering a fatal error.

        bug
                *BUG()* on a fatal error, the system will stay in the crashed state and may be
                still partially usable, but reboot is required for full operation
        panic
                *panic()* on a fatal error, depending on other system configuration, this may
                be followed by a reboot. Please refer to the documentation of kernel boot
                parameters, eg. *panic*, *oops* or *crashkernel*.

flushoncommit, noflushoncommit
        (default: off)

        This option forces any data dirtied by a write in a prior transaction to commit
        as part of the current commit, effectively a full filesystem sync.

        This makes the committed state a fully consistent view of the file system from
        the application's perspective (i.e. it includes all completed file system
        operations). This was previously the behavior only when a snapshot was
        created.

        When off, the filesystem is consistent but buffered writes may last more than
        one transaction commit.

fragment=<type>
        (depends on compile-time option BTRFS_DEBUG, since: 4.4, default: off)

        A debugging helper to intentionally fragment given *type* of block groups. The
        type can be *data*, *metadata* or *all*. This mount option should not be used
        outside of debugging environments and is not recognized if the kernel config
        option *BTRFS_DEBUG* is not enabled.

nologreplay
        (default: off, even read-only)

        The tree-log contains pending updates to the filesystem until the full commit.
        The log is replayed on next mount, this can be disabled by this option.  See
        also *treelog*.  Note that *nologreplay* is the same as *norecovery*.

        .. warning::
                Currently, the tree log is replayed even with a read-only mount! To
                disable that behaviour, mount also with *nologreplay*.

max_inline=<bytes>
        (default: min(2048, page size) )

        Specify the maximum amount of space, that can be inlined in
        a metadata b-tree leaf.  The value is specified in bytes, optionally
        with a K suffix (case insensitive).  In practice, this value
        is limited by the filesystem block size (named *sectorsize* at mkfs time),
        and memory page size of the system. In case of sectorsize limit, there's
        some space unavailable due to leaf headers.  For example, a 4KiB sectorsize,
        maximum size of inline data is about 3900 bytes.

        Inlining can be completely turned off by specifying 0. This will increase data
        block slack if file sizes are much smaller than block size but will reduce
        metadata consumption in return.

        .. note::
                The default value has changed to 2048 in kernel 4.6.

metadata_ratio=<value>
        (default: 0, internal logic)

        Specifies that 1 metadata chunk should be allocated after every *value* data
        chunks. Default behaviour depends on internal logic, some percent of unused
        metadata space is attempted to be maintained but is not always possible if
        there's not enough space left for chunk allocation. The option could be useful to
        override the internal logic in favor of the metadata allocation if the expected
        workload is supposed to be metadata intense (snapshots, reflinks, xattrs,
        inlined files).

norecovery
        (since: 4.5, default: off)

        Do not attempt any data recovery at mount time. This will disable *logreplay*
        and avoids other write operations. Note that this option is the same as
        *nologreplay*.


        .. note::
                The opposite option *recovery* used to have different meaning but was
                changed for consistency with other filesystems, where *norecovery* is used for
                skipping log replay. BTRFS does the same and in general will try to avoid any
                write operations.

rescan_uuid_tree
        (since: 3.12, default: off)

        Force check and rebuild procedure of the UUID tree. This should not
        normally be needed.

rescue
        (since: 5.9)

        Modes allowing mount with damaged filesystem structures.

        * *usebackuproot* (since: 5.9, replaces standalone option *usebackuproot*)
        * *nologreplay* (since: 5.9, replaces standalone option *nologreplay*)
        * *ignorebadroots*, *ibadroots* (since: 5.11)
        * *ignoredatacsums*, *idatacsums* (since: 5.11)
        * *all* (since: 5.9)

skip_balance
        (since: 3.3, default: off)

        Skip automatic resume of an interrupted balance operation. The operation can
        later be resumed with **btrfs balance resume**, or the paused state can be
        removed with **btrfs balance cancel**. The default behaviour is to resume an
        interrupted balance immediately after a volume is mounted.

space_cache, space_cache=<version>, nospace_cache
        (*nospace_cache* since: 3.2, *space_cache=v1* and *space_cache=v2* since 4.5, default: *space_cache=v1*)

        Options to control the free space cache. The free space cache greatly improves
        performance when reading block group free space into memory. However, managing
        the space cache consumes some resources, including a small amount of disk
        space.

        There are two implementations of the free space cache. The original
        one, referred to as *v1*, is the safe default. The *v1* space cache can be
        disabled at mount time with *nospace_cache* without clearing.

        On very large filesystems (many terabytes) and certain workloads, the
        performance of the *v1* space cache may degrade drastically. The *v2*
        implementation, which adds a new b-tree called the free space tree, addresses
        this issue. Once enabled, the *v2* space cache will always be used and cannot
        be disabled unless it is cleared. Use *clear_cache,space_cache=v1* or
        *clear_cache,nospace_cache* to do so. If *v2* is enabled, kernels without *v2*
        support will only be able to mount the filesystem in read-only mode.

        The ``btrfs-check(8)`` and ```mkfs.btrfs(8)`` commands have full *v2* free space
        cache support since v4.19.

        If a version is not explicitly specified, the default implementation will be
        chosen, which is *v1*.

ssd, ssd_spread, nossd, nossd_spread
        (default: SSD autodetected)

        Options to control SSD allocation schemes.  By default, BTRFS will
        enable or disable SSD optimizations depending on status of a device with
        respect to rotational or non-rotational type. This is determined by the
        contents of */sys/block/DEV/queue/rotational*). If it is 0, the *ssd* option is
        turned on.  The option *nossd* will disable the autodetection.

        The optimizations make use of the absence of the seek penalty that's inherent
        for the rotational devices. The blocks can be typically written faster and
        are not offloaded to separate threads.

        .. note::
                Since 4.14, the block layout optimizations have been dropped. This used
                to help with first generations of SSD devices. Their FTL (flash translation
                layer) was not effective and the optimization was supposed to improve the wear
                by better aligning blocks. This is no longer true with modern SSD devices and
                the optimization had no real benefit. Furthermore it caused increased
                fragmentation. The layout tuning has been kept intact for the option
                *ssd_spread*.

        The *ssd_spread* mount option attempts to allocate into bigger and aligned
        chunks of unused space, and may perform better on low-end SSDs.  *ssd_spread*
        implies *ssd*, enabling all other SSD heuristics as well. The option *nossd*
        will disable all SSD options while *nossd_spread* only disables *ssd_spread*.

subvol=<path>
        Mount subvolume from *path* rather than the toplevel subvolume. The
        *path* is always treated as relative to the toplevel subvolume.
        This mount option overrides the default subvolume set for the given filesystem.

subvolid=<subvolid>
        Mount subvolume specified by a *subvolid* number rather than the toplevel
        subvolume.  You can use **btrfs subvolume list** of **btrfs subvolume show** to see
        subvolume ID numbers.
        This mount option overrides the default subvolume set for the given filesystem.

        .. note::
                If both *subvolid* and *subvol* are specified, they must point at the
                same subvolume, otherwise the mount will fail.

thread_pool=<number>
        (default: min(NRCPUS + 2, 8) )

        The number of worker threads to start. NRCPUS is number of on-line CPUs
        detected at the time of mount. Small number leads to less parallelism in
        processing data and metadata, higher numbers could lead to a performance hit
        due to increased locking contention, process scheduling, cache-line bouncing or
        costly data transfers between local CPU memories.

treelog, notreelog
        (default: on)

        Enable the tree logging used for *fsync* and *O_SYNC* writes. The tree log
        stores changes without the need of a full filesystem sync. The log operations
        are flushed at sync and transaction commit. If the system crashes between two
        such syncs, the pending tree log operations are replayed during mount.

        .. warning::
                Currently, the tree log is replayed even with a read-only mount! To
                disable that behaviour, also mount with *nologreplay*.

        The tree log could contain new files/directories, these would not exist on
        a mounted filesystem if the log is not replayed.

usebackuproot
        (since: 4.6, default: off)

        Enable autorecovery attempts if a bad tree root is found at mount time.
        Currently this scans a backup list of several previous tree roots and tries to
        use the first readable. This can be used with read-only mounts as well.

        .. note::
                This option has replaced *recovery*.

user_subvol_rm_allowed
        (default: off)

        Allow subvolumes to be deleted by their respective owner. Otherwise, only the
        root user can do that.

        .. note::
                Historically, any user could create a snapshot even if he was not owner
                of the source subvolume, the subvolume deletion has been restricted for that
                reason. The subvolume creation has been restricted but this mount option is
                still required. This is a usability issue.
                Since 4.18, the ``rmdir(2)`` syscall can delete an empty subvolume just like an
                ordinary directory. Whether this is possible can be detected at runtime, see
                *rmdir_subvol* feature in *FILESYSTEM FEATURES*.

DEPRECATED MOUNT OPTIONS
^^^^^^^^^^^^^^^^^^^^^^^^

List of mount options that have been removed, kept for backward compatibility.

recovery
        (since: 3.2, default: off, deprecated since: 4.5)

        .. note::
                This option has been replaced by *usebackuproot* and should not be used
                but will work on 4.5+ kernels.

inode_cache, noinode_cache
        (removed in: 5.11, since: 3.0, default: off)

        .. note::
                The functionality has been removed in 5.11, any stale data created by
                previous use of the *inode_cache* option can be removed by **btrfs check
                --clear-ino-cache**.


NOTES ON GENERIC MOUNT OPTIONS
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Some of the general mount options from ``mount(8)`` that affect BTRFS and are
worth mentioning.

noatime
        under read intensive work-loads, specifying *noatime* significantly improves
        performance because no new access time information needs to be written. Without
        this option, the default is *relatime*, which only reduces the number of
        inode atime updates in comparison to the traditional *strictatime*. The worst
        case for atime updates under 'relatime' occurs when many files are read whose
        atime is older than 24 h and which are freshly snapshotted. In that case the
        atime is updated and COW happens - for each file - in bulk. See also
        https://lwn.net/Articles/499293/ - *Atime and btrfs: a bad combination? (LWN, 2012-05-31)*.

        Note that *noatime* may break applications that rely on atime uptimes like
        the venerable Mutt (unless you use maildir mailboxes).



