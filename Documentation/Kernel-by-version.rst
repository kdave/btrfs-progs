Changes (kernel/version)
========================

Summary of kernel changes for each version.

6.0 (Oct 2022)
--------------

Pull requests:
`v6.0-rc1 <https://git.kernel.org/linus/353767e4aaeb7bc818273dfacbb01dd36a9db47a>`__,
`v6.0-rc1 <https://git.kernel.org/linus/2e4f8c729db5f3c0b8ea8b1b99f1ae124152e8cc>`__,
`v6.0-rc2 <https://git.kernel.org/linus/42c54d5491ed7b9fe89a499224494277a33b23df>`__,
`v6.0-rc3 <https://git.kernel.org/linus/8379c0b31fbc5d20946f617f8e2fe4791e6f58c1>`__,
`v6.0-rc5 <https://git.kernel.org/linus/9b4509495418a0effe964b0aad9a522be5a3b6d5>`__,
`v6.0-rc7 <https://git.kernel.org/linus/60891ec99e141b74544d11e897a245ef06263052>`__

- sysfs updates:

  - export chunk size, in debug mode add tunable for setting its size
  - show zoned among features (was only in debug mode)
  - show commit stats (number, last/max/total duration)
  - mixed_backref and big_metadata sysfs feature files removed, they've
    been default for sufficiently long time, there are no known users and
    mixed_backref could be confused with mixed_groups

- send protocol updated to version 2

  - new commands:

    - ability write larger data chunks than 64K
    - send raw compressed extents (uses the encoded data ioctls), ie. no
      decompression on send side, no compression needed on receive side
      if supported
    - send 'otime' (inode creation time) among other timestamps
    - send file attributes (a.k.a file flags and xflags)

  - this is first version bump, backward compatibility on send and
    receive side is provided
  - there are still some known and wanted commands that will be
    implemented in the near future, another version bump will be needed,
    however we want to minimize that to avoid causing usability issues

- print checksum type and implementation at mount time
- don't print some messages at mount (mentioned as people asked about
  it), we want to print messages namely for new features so let's make
  some space for that:

  - big metadata - this has been supported for a long time and is not a feature
    that's worth mentioning
  - skinny metadata - same reason, set by default by mkfs

Performance improvements:

-  reduced amount of reserved metadata for delayed items

   -  when inserted items can be batched into one leaf
   -  when deleting batched directory index items
   -  when deleting delayed items used for deletion
   -  overall improved count of files/sec, decreased subvolume lock
      contention

- metadata item access bounds checker micro-optimized, with a few
  percent of improved runtime for metadata-heavy operations
- increase direct io limit for read to 256 sectors, improved throughput
  by 3x on sample workload

Notable fixes:

- raid56

  - reduce parity writes, skip sectors of stripe when there are no data updates
  - restore reading from stripe cache instead of triggering new read

- refuse to replay log with unknown incompat read-only feature bit set
- tree-checker verifies if extent items don't overlap
- check that subvolume is writable when changing xattrs from security
  namespace
- fix space cache corruption and potential double allocations; this is
  a rare bug but can be serious once it happens, stable backports and
  analysis tool will be provided

- zoned:

  - fix page locking when COW fails in the middle of allocation
  - improved tracking of active zones, ZNS drives may limit the number
    and there are ENOSPC errors due to that limit and not actual lack of
    space
  - adjust maximum extent size for zone append so it does not cause late
    ENOSPC due to underreservation

- mirror reading error messages show the mirror number
- don't fallback to buffered IO for NOWAIT direct IO writes, we don't
  have the NOWAIT semantics for buffered io yet
- send, fix sending link commands for existing file paths when there are
  deleted and created hardlinks for same files
- repair all mirrors for profiles with more than 1 copy (raid1c34)
- fix repair of compressed extents, unify where error detection and
  repair happen

6.1 (Dec 2022)
--------------

Pull requests:
`v6.1-rc1 <https://git.kernel.org/linus/76e45035348c247a70ed50eb29a9906657e4444f>`__,
`v6.1-rc1 <https://git.kernel.org/linus/7f198ba7ae9874c64ffe8cd3aa60cf5dab78ce3a>`__,
`v6.1-rc2 <https://git.kernel.org/linus/aae703b02f92bde9264366c545e87cec451de471>`__,
`v6.1-rc4 <https://git.kernel.org/linus/5aaef24b5c6d4246b2cac1be949869fa36577737>`__,
`v6.1-rc4 <https://git.kernel.org/linus/f2f32f8af2b0ca9d619e5183eae3eed431793baf>`__,
`v6.1-rc5 <https://git.kernel.org/linus/1767a722a708f1fa3b9af39eb091d79101f8c086>`__,
`v6.1-rc7 <https://git.kernel.org/linus/3eaea0db25261f62e21229f5763728dac40a1058>`__

Performance:

- outstanding FIEMAP speed improvements:

  - algorithmic change how extents are enumerated leads to orders of
    magnitude speed boost (uncached and cached)
  - extent sharing check speedup (2.2x uncached, 3x cached)
  - add more cancellation points, allowing to interrupt seeking in files
    with large number of extents
  - more efficient hole and data seeking (4x uncached, 1.3x cached)
  - sample results:
    256M, 32K extents:   4s ->  29ms  (~150x)
    512M, 64K extents:  30s ->  59ms  (~550x)
    1G,  128K extents: 225s -> 120ms (~1800x)

- improved inode logging, especially for directories (on dbench workload
  throughput +25%, max latency -21%)
- improved buffered IO, remove redundant extent state tracking, lowering
  memory consumption and avoiding rb tree traversal
- add sysfs tunable to let qgroup temporarily skip exact accounting when
  deleting snapshot, leading to a speedup but requiring a rescan after
  that, will be used by snapper
- support io_uring and buffered writes, until now it was just for direct
  IO, with the no-wait semantics implemented in the buffered write path
  it now works and leads to speed improvement in IOPS (2x), throughput
  (2.2x), latency (depends, 2x to 150x)
- small performance improvements when dropping and searching for extent
  maps as well as when flushing delalloc in COW mode (throughput +5MB/s)

User visible changes:

- new incompatible feature block-group-tree adding a dedicated tree for
  tracking block groups, this allows a much faster load during mount and
  avoids seeking unlike when it's scattered in the extent tree items

  - this reduces mount time for many-terabyte sized filesystems
  - conversion tool will be provided so existing filesystem can also be
    updated in place
  - to reduce test matrix and feature combinations requires no-holes
    and free-space-tree (mkfs defaults since 5.15)

- improved reporting of super block corruption detected by scrub
- scrub also tries to repair super block and does not wait until next
  commit
- discard stats and tunables are exported in sysfs
  (/sys/fs/btrfs/FSID/discard)
- qgroup status is exported in sysfs (/sys/sys/fs/btrfs/FSID/qgroups/)
- verify that super block was not modified when thawing filesystem

Fixes:

- FIEMAP fixes:

  - fix extent sharing status, does not depend on the cached status where merged
  - flush delalloc so compressed extents are reported correctly

- fix alignment of VMA for memory mapped files on THP
- send: fix failures when processing inodes with no links (orphan files
  and directories)
- handle more corner cases for read-only compat feature verification
- fix crash on raid0 filesystems created with <5.4 mkfs.btrfs that could
  lead to division by zero

Core:

- preliminary support for fs-verity in send
- more effective memory use in scrub for subpage where sector is smaller
  than page
- block group caching progress logic has been removed, load is now
  synchronous
- add no-wait semantics to several functions (tree search, nocow,
  flushing, buffered write

6.2 (Feb 2023)
--------------

Pull requests:
`v6.2-rc1 <https://git.kernel.org/linus/149c51f876322d9bfbd5e2d6ffae7aff3d794384>`__,
`v6.2-rc3 <https://git.kernel.org/linus/69b41ac87e4a664de78a395ff97166f0b2943210>`__,
`v6.2-rc3 <https://git.kernel.org/linus/fc7b76c4a4d139ebcae2af3bd75215fc90834e3b>`__,
`v6.2-rc5 <https://git.kernel.org/linus/d532dd102151cc69fcd00b13e5a9689b23c0c8d9>`__,
`v6.2-rc5 <https://git.kernel.org/linus/7026172bc334300652cb36d59b392c1a6b20926a>`__,
`v6.2-rc5 <https://git.kernel.org/linus/26e57507a0f04ae0e472afe4799784e2ed19e1b0>`__,
`v6.2-rc8 <https://git.kernel.org/linus/66fcf74e5c0d771a456b96ec9aebfb53d648eede>`__,
`v6.2-rc8 <https://git.kernel.org/linus/711e9a4d52bf4e477e51c7135e1e6188c42018d0>`__

User visible features:

- raid56 reliability vs performance trade off:

  - fix destructive RMW for raid5 data (raid6 still needs work) - do full RMW
    cycle for writes and verify all checksums before overwrite, this should
    prevent rewriting potentially corrupted data without notice
  - stripes are cached in memory which should reduce the performance impact but
    still can hurt some workloads
  - checksums are verified after repair again
  - this is the last option without introducing additional features (write
    intent bitmap, journal, another tree), the RMW cycle was supposed to be
    avoided by the original implementation exactly for performance reasons but
    that caused all the reliability problems

- discard=async by default for devices that support it
- implement emergency flush reserve to avoid almost all unnecessary transaction
  aborts due to ENOSPC in cases where there are too many delayed refs or
  delayed allocation
- skip block group synchronization if there's no change in used bytes, can
  reduce transaction commit count for some workloads
- print more specific errors to system log when device scan ioctl fails

Performance improvements:

- fiemap and lseek:

  - overall speedup due to skipping unnecessary or duplicate searches (-40% run time)
  - cache some data structures and sharedness of extents (-30% run time)

- send:

  - faster backref resolution when finding clones
  - cached leaf to root mapping for faster backref walking
  - improved clone/sharing detection
  - overall run time improvements (-70%)

Fixes:

- fix compat ro feature check at read-write remount
- handle case when read-repair happens with ongoing device replace
- reset defrag ioctl buffer on memory allocation error
- fix potential crash in quota when rescan races with disable
- fix qgroup accounting warning when rescan can be started at time with
  temporarily disabled accounting
- don't cache a single-device filesystem device to avoid cases when a
  loop device is reformatted and the entry gets stale
- limit number of send clones by maximum memory allocated

6.3 (Apr 2023)
--------------

Pull requests:
`v6.3-rc1 <https://git.kernel.org/linus/885ce48739189fac6645ff42d736ee0de0b5917d>`__,
`v6.3-rc2 <https://git.kernel.org/linus/ae195ca1a8a4af75073e82c485148897c923f88f>`__,
`v6.3-rc4 <https://git.kernel.org/linus/285063049a65251aada1c34664de692dd083aa03>`__,
`v6.3-rc5 <https://git.kernel.org/linus/6ab608fe852b50fe809b22cdf7db6cbe006d7cb3>`__,
`v6.3-rc7 <https://git.kernel.org/linus/2c40519251d61590377b313379ae2d4d4ef28266>`__,
`v6.3 <https://git.kernel.org/linus/c337b23f32c87320dffd389e4f0f793db35f0a9b>`__

Features:

- block group allocation class heuristics:

  - pack files by size (up to 128k, up to 8M, more) to avoid
    fragmentation in block groups, assuming that file size and life time
    is correlated, in particular this may help during balance
  - with tracepoints and extensible in the future

- sysfs export of per-device fsid in DEV_INFO ioctl to distinguish seeding
  devices, needed for testing
- print sysfs stats for the allocation classes

Performance:

- send: cache directory utimes and only emit the command when necessary

  - speedup up to 10x
  - smaller final stream produced (no redundant utimes commands issued),
  - compatibility not affected

- fiemap:

  - skip backref checks for shared leaves
  - speedup 3x on sample filesystem with all leaves shared (e.g. on
    snapshots)

- micro optimized b-tree key lookup, speedup in metadata operations
  (sample benchmark: fs_mark +10% of files/sec)

Core changes:

- change where checksumming is done in the io path

  - checksum and read repair does verification at lower layer
  - cascaded cleanups and simplifications

Fixes:

- sysfs: make sure that a run-time change of a feature is correctly
  tracked by the feature files
- scrub: better reporting of tree block errors
- fix calculation of unusable block group space reporting bogus values
  due to 32/64b division
- fix unnecessary increment of read error stat on write error
- scan block devices in non-exclusive mode to avoid temporary mkfs
  failures
- fix fast checksum detection, this affects filesystems with non-crc32c
  checksum, calculation would not be offloaded to worker threads (since 5.4)
- restore thread_pool mount option behaviour for endio workers, the
  new value for maximum active threads would not be set to the actual
  work queues (since 6.0)

6.4 (Jun 2023)
--------------

Pull requests:
`v6.4-rc1 <https://git.kernel.org/linus/85d7ab2463822a4ab096c0b7b59feec962552572>`__,
`v6.4-rc2 <https://git.kernel.org/linus/1dc3731daf1f350cfd631b5559aac865ab2fbb4c>`__,
`v6.4-rc2 <https://git.kernel.org/linus/76c7f8873a7696dbd8f9cd844e30e5c84cbaba1a>`__,
`v6.4-rc4 <https://git.kernel.org/linus/b158dd941b4f28e12c4f956caf2352febe09fe4e>`__,
`v6.4-rc5 <https://git.kernel.org/linus/48b1320a674e1ff5de2fad8606bee38f724594dc>`__,
`v6.4-rc5 <https://git.kernel.org/linus/e0178b546d24f42a85f4d4da080fb801e0d49107>`__,
`v6.4-rc7 <https://git.kernel.org/linus/ace9e12da2f09faf85cd1904c14e1ab3ca49a590>`__,
`v6.4-rc7 <https://git.kernel.org/linus/4973ca29552864a7a047ab8a15611a585827412f>`__,
`v6.4 <https://git.kernel.org/linus/4b0c7a1ba09386e26cf9e55cd375af8e0f48662e>`__,
`v6.4 <https://git.kernel.org/linus/569fa9392d2d48e35955b69775d11507ea96b36a>`__

Performance improvements:

-  improve logging changes in a directory during one transaction, avoid
   iterating over items and reduce lock contention (fsync time 4x lower)
-  when logging directory entries during one transaction, reduce locking
   of subvolume trees by checking tree-log instead (improvement in
   throughput and latency for concurrent access to a subvolume)

Notable fixes:

-  device replace:

   -  properly honor read mode when requested to avoid reading from source device
   -  target device won't be used for eventual read repair, this is
      unreliable for NODATASUM files
   -  when there are unpaired (and unrepairable) metadata during replace,
      exit early with error and don't try to finish whole operation
-  scrub ioctl properly rejects unknown flags
-  fix partial direct io write when there's a page fault in the middle,
   iomap will try to continue with partial request but the btrfs part did
   not match that, this can lead to zeros written instead of data
-  fix backref walking, this breaks a mode of LOGICAL_INO_V2 ioctl that
   is used in deduplication tools
-  make mount option clear_cache work with block-group-tree, to rebuild
   free-space-tree instead of temporarily disabling it that would lead to
   a forced read-only mount

Core changes:

-  io path

   -  continued cleanups and refactoring around bio handling
   -  extent io submit path simplifications and cleanups
   -  flush write path simplifications and cleanups
   -  rework logic of passing sync mode of bio, with further cleanups
-  rewrite scrub code flow, restructure how the stripes are enumerated
   and verified in a more unified way
-  allow to set lower threshold for block group reclaim in debug mode to
   aid zoned mode testing
-  remove obsolete time-based delayed ref throttling logic when
   truncating items

6.5 (Aug 2023)
--------------

Pull requests:
`v6.5-rc1 <https://git.kernel.org/linus/cc423f6337d0a5ff1906f3b3d465d28c0d1705f6>`__,
`v6.5-rc3 <https://git.kernel.org/linus/46670259519f4ee4ab378dc014798aabe77c5057>`__,
`v6.5-rc4 <https://git.kernel.org/linus/64de76ce8e26fb0a5ca32ac2210ef99238c28525>`__,
`v6.5-rc6 <https://git.kernel.org/linus/a785fd28d31f76d50004712b6e0b409d5a8239d8>`__,
`v6.5-rc7 <https://git.kernel.org/linus/12e6ccedb311b32b16f767fdd606cc84630e45ae>`__

Performance improvements:

-  speedup in fsync(), better tracking of inode logged status can avoid
   transaction commit
-  IO path structures track logical offsets in data structures and does
   not need to look it up
-  submit IO synchronously for fast checksums (crc32c and xxhash), remove
   high priority worker kthread

User visible changes:

-  don't commit transaction for every created subvolume, this can reduce
   time when many subvolumes are created in a batch
-  print affected files when relocation fails
-  trigger orphan file cleanup during START_SYNC ioctl
-  the ``async=discard`` has been enabled in 6.2 unconditionally, but for
   zoned mode it does not make that much sense to do it asynchronously as
   the zones are reset as needed

6.6 (Oct 2023)
--------------

Pull requests:
`v6.6-rc1 <https://git.kernel.org/linus/547635c6ac47c7556d6954935b189defe90422f7>`__,
`v6.6-rc2 <https://git.kernel.org/linus/3669558bdf354cd352be955ef2764cde6a9bf5ec>`__,
`v6.6-rc3 <https://git.kernel.org/linus/a229cf67ab851a6e92395f37ed141d065176575a>`__,
`v6.6-rc4 <https://git.kernel.org/linus/cac405a3bfa21a6e17089ae2f355f34594bfb543>`__,
`v6.6-rc5 <https://git.kernel.org/linus/7de25c855b63453826ef678420831f98331d85fd>`__,
`v6.6-rc6 <https://git.kernel.org/linus/759d1b653f3c7c2249b7fe5f6b218f87a5842822>`__,
`v6.6-rc7 (1) <https://git.kernel.org/linus/7cf4bea77ab60742c128c2ceb4b1b8078887b823>`__,
`v6.6-rc8 (2) <https://git.kernel.org/linus/e017769f4ce20dc0d3fa3220d4d359dcc4431274>`__,

Notable fixes:

- scrub performance drop due to rewrite in 6.4 partially restored, the drop is
  noticeable on fast PCIe devices, -66% and restored to -33% of the original
- copy directory permissions and time when creating a stub subvolume
- fix transaction commit stalls when auto relocation is running and blocks
  other tasks that want to commit
- change behaviour of readdir()/rewinddir() when new directory entries are
  created after opendir(), properly tracking the last entry

Core:

- debugging feature integrity checker deprecated, to be removed in 6.7
- in zoned mode, zones are activated just before the write, making
  error handling easier, now the overcommit mechanism can be enabled
  again which improves performance by avoiding more frequent flushing
- v0 extent handling completely removed, deprecated long time ago

6.7 (Jan 2024)
--------------

Pull requests:
`v6.7-rc1 <https://git.kernel.org/linus/d5acbc60fafbe0fc94c552ce916dd592cd4c6371>`__,
`v6.7-rc2 <https://git.kernel.org/linus/9bacdd8996c77c42ca004440be610692275ff9d0>`__,
`v6.7-rc4 <https://git.kernel.org/linus/18d46e76d7c2eedd8577fae67e3f1d4db25018b0>`__,
`v6.7-rc6 (1) <https://git.kernel.org/linus/bdb2701f0b6822d711ec34968ccef70b73a91da7>`__,
`v6.7-rc6 (2) <https://git.kernel.org/linus/0e389834672c723435a44818ed2cabc4dad24429>`__,

New features:

- raid-stripe-tree:

  - New tree for logical file extent mapping where the physical mapping may not
    match on multiple devices. This is now used in zoned mode to implement
    RAID0/RAID1* profiles, but can be used in non-zoned mode as well. The
    support for RAID56 is in development and will eventually fix the problems
    with the current implementation. This is a backward incompatible feature
    and has to be enabled at mkfs time.

- simple quota accounting (squota):

  - A simplified mode of qgroup that accounts all space on the initial extent
    owners (a subvolume), the snapshots are then cheap to create and delete.
    The deletion of snapshots in fully accounting qgroups is a known CPU/IO
    performance bottleneck.

  - Note: The squota is not suitable for the general use case but works well
    for containers where the original subvolume exists for the whole time. This
    is a backward incompatible feature as it needs extending some structures,
    but can be enabled on an existing filesystem.

- temporary filesystem fsid (temp_fsid):

  - The fsid identifies a filesystem and is hard coded in the structures, which
    disallows mounting the same fsid found on different devices.

  - For a single device filesystem this is not strictly necessary, a new
    temporary fsid can be generated on mount e.g. after a device is cloned.
    This will be used by Steam Deck for root partition A/B testing, or can be
    used for VM root images.

- filesystems with partially finished metadata_uuid conversion cannot be
  mounted anymore and the uuid fixup has to be done by btrfs-progs (btrfstune).

Performance improvements:

- reduce reservations for checksum deletions (with enabled free space tree by
  factor of 4), on a sample workload on file with many extents the deletion
  time decreased by 12%

- make extent state merges more efficient during insertions, reduce rb-tree
  iterations (run time of critical functions reduced by 5%)

Core changes:

- the integrity check functionality has been removed, this was a debugging
  feature and removal does not affect other integrity checks like checksums or
  tree-checker

-  space reservation changes:

   - more efficient delayed ref reservations, this avoids building up too much
     work or overusing or exhausting the global block reserve in some situations
   - move delayed refs reservation to the transaction start time, this prevents
     some ENOSPC corner cases related to exhaustion of global reserve

   - adjust overcommit logic in near full situations, account for one more
     chunk to eventually allocate metadata chunk, this is mostly relevant for
     small filesystems (<10GiB)

- single device filesystems are scanned but not registered (except seed
  devices), this allows temp_fsid to work

6.8 (Mar 2024)
--------------

Pull requests:
`v6.8-rc1 <https://git.kernel.org/linus/affc5af36bbb62073b6aaa4f4459b38937ff5331>`__,
`v6.8-rc2 <https://git.kernel.org/linus/5d9248eed48054bf26b3d5ad3d7073a356a17d19>`__,
`v6.8-rc4 <https://git.kernel.org/linus/6d280f4d760e3bcb4a8df302afebf085b65ec982>`__,
`v6.8-rc5 <https://git.kernel.org/linus/1f3a3e2aaeb4e6ba9b6df6f2e720131765b23b82>`__,
`v6.8-rc6 <https://git.kernel.org/linus/8da8d88455ebbb4e05423cf60cff985e92d43754>`__,
`v6.8-rc7 (1) <https://git.kernel.org/linus/b6c1f1ecb3bf2dcd8085cc7d927ade623182a26c>`__,
`v6.8-rc7 (2) <https://git.kernel.org/linus/7505aa147adb10913c1b72e947006b6070753eb6>`__

Core changes:

-  convert extent buffers to folios:

   - direct API conversion where possible
   - performance can drop by a few percent on metadata heavy
     workloads, the folio sizes are not constant and the calculations
     add up in the item helpers
   - both regular and subpage modes
   - data cannot be converted yet, we need to port that to iomap and
     there are some other generic changes required

-  convert mount to the new API, should not be user visible:

   - options deprecated long time ago have been removed: inode_cache,
     recovery
   - the new logic that splits mount to two phases slightly changes
     timing of device scanning for multi-device filesystems
   - LSM options will now work (like for selinux)

- convert delayed nodes radix tree to xarray, preserving the
  preload-like logic that still allows to allocate with GFP_NOFS

Performance improvements:

- refactor chunk map structure, reduce size and improve performance

- extent map refactoring, smaller data structures, improved performance

- reduce size of struct extent_io_tree, embedded in several structures

- temporary pages used for compression are cached and attached to a shrinker,
  this may slightly improve performance

Fixes:

- fix over-reservation of metadata chunks due to not keeping proper balance
  between global block reserve and delayed refs reserve; in practice this
  leaves behind empty metadata block groups, the workaround is to reclaim them
  by using the '-musage=1' balance filter

- fix corner case of send that would generate potentially large stream of zeros
  if there's a hole at the end of the file

- fix chunk validation in zoned mode on conventional zones, it was possible to
  create chunks that would not be allowed on sequential zones

6.9 (May 2024)
--------------

Pull requests:
`v6.9-rc1 (1) <https://git.kernel.org/linus/43a7548e28a6df12a6170421d9d016c576010baa>`__,
`v6.9-rc1 (2) <https://git.kernel.org/linus/7b65c810a1198b91ed6bdc49ddb470978affd122>`__,
`v6.9-rc2 <https://git.kernel.org/linus/400dd456bda8be0b566f2690c51609ea02f85766>`__,
`v6.9-rc3 <https://git.kernel.org/linus/20cb38a7af88dc40095da7c2c9094da3873fea23>`__,
`v6.9-rc5 <https://git.kernel.org/linus/8cd26fd90c1ad7acdcfb9f69ca99d13aa7b24561>`__,
`v6.9-rc6 <https://git.kernel.org/linus/e88c4cfcb7b888ac374916806f86c17d8ecaeb67>`__,
`v6.9-rc7 <https://git.kernel.org/linus/f03359bca01bf4372cf2c118cd9a987a5951b1c8>`__,
`v6.9-rc8 <https://git.kernel.org/linus/dccb07f2914cdab2ac3a5b6c98406f765acab803>`__,

Performance improvements:

- minor speedup in logging when repeatedly allocated structure is preallocated
  only once, improves latency and decreases lock contention

- minor throughput increase (+6%), reduced lock contention after clearing
  delayed allocation bits, applies to several common workload types

- features under CONFIG_BTRFS_DEBUG:

  - sysfs knob for setting the how checksums are calculated when submitting IO,
    inline or offloaded to a thread, this affects latency and throughput on some
    block group profiles

Notable fixes:

- fix device tracking in memory that broke grub-probe

- zoned mode fixes:

  - use zone-aware super block access during scrub
  - delete zones that are 100% unusable to reclaim space

Other notable changes:

- additional validation of devices by major:minor numbers

6.10 (Jul 2024)
---------------

Pull requests:
`v6.10-rc1 (1) <https://git.kernel.org/linus/a3d1f54d7aa4c3be2c6a10768d4ffa1dcb620da9>`__,
`v6.10-rc1 (2) <https://git.kernel.org/linus/02c438bbfffeabf8c958108f9cf88cdb1a11a323>`__,
`v6.10-rc3 (1) <https://git.kernel.org/linus/19ca0d8a433ff37018f9429f7e7739e9f3d3d2b4>`__,
`v6.10-rc3 (2) <https://git.kernel.org/linus/07978330e63456a75a6d5c1c5053de24bdc9d16f>`__,
`v6.10-rc5 <https://git.kernel.org/linus/50736169ecc8387247fe6a00932852ce7b057083>`__,
`v6.10-rc6 <https://git.kernel.org/linus/66e55ff12e7391549c4a85a7a96471dcf891cb03>`__,
`v6.10-rc7 (1) <https://git.kernel.org/linus/cfbc0ffea88c764d23f69efe6ecb74918e0f588e>`__,
`v6.10-rc7 (2) <https://git.kernel.org/linus/661e504db04c6b7278737ee3a9116738536b4ed4>`__,
`v6.10-rc8 <https://git.kernel.org/linus/975f3b6da18020f1c8a7667ccb08fa542928ec03>`__,

Performance improvements:

- inline b-tree locking functions, improvement in metadata-heavy changes

- relax locking on a range that's being reflinked, allows read operations to
  run in parallel

- speed up NOCOW write checks (throughput +9% on a sample test)

- extent locking ranges have been reduced in several places, namely around
  delayed ref processing

Notable fixes or changes:

- add back mount option *norecovery*, deprecated long time ago and removed in
  6.8 but still in use

- fix potential infinite loop when doing block group reclaim

- extent map shrinker, allow memory consumption reduction for direct io loads

6.11 (Sep 2024)
---------------

Pull requests:
`v6.11-rc1 (1) <https://git.kernel.org/linus/a1b547f0f217cfb06af7eb4ce8488b02d83a0370>`__,
`v6.11-rc1 (2) <https://git.kernel.org/linus/53a5182c8a6805d3096336709ba5790d16f8c369>`__,
`v6.11-rc2 <https://git.kernel.org/linus/e4fc196f5ba36eb7b9758cf2c73df49a44199895>`__,
`v6.11-rc3 <https://git.kernel.org/linus/6a0e38264012809afa24113ee2162dc07f4ed22b>`__,
`v6.11-rc4 <https://git.kernel.org/linus/1fb918967b56df3262ee984175816f0acb310501>`__,
`v6.11-rc4 <https://git.kernel.org/linus/57b14823ea68592bd67e4992a2bf0dd67abb68d6>`__,
`v6.11-rc6 <https://git.kernel.org/linus/2840526875c7e3bcfb3364420b70efa203bad428>`__,
`v6.11-rc7 <https://git.kernel.org/linus/1263a7bf8a0e77c6cda8f5a40509d99829216a45>`__,

User visible features:

- dynamic block group reclaim:

  - tunable framework to avoid situations where eager data allocations prevent
    creating new metadata chunks due to lack of unallocated space
  - reuse sysfs knob bg_reclaim_threshold (otherwise used only in zoned mode)
    for a fixed value threshold
  - new on/off sysfs knob "dynamic_reclaim" calculating the value based on
    heuristics, aiming to keep spare working space for relocating chunks but
    not to needlessly relocate partially utilized block groups or reclaim newly
    allocated ones
  - stats are exported in sysfs per block group type, files "reclaim_*"
  - this may increase IO load at unexpected times but the corner case of no
    allocatable block groups is known to be worse

- automatically remove qgroup of deleted subvolumes:

  - adjust qgroup removal conditions, make sure all related subvolume data are
    already removed, or return EBUSY, also take into account setting of sysfs
    drop_subtree_threshold
  - also works in squota mode

- mount option updates: new modes of 'rescue=' that allow to mount images
  (read-only) that could have been partially converted by user space tools

  - ignoremetacsums  - invalid metadata checksums are ignored
  - ignoresuperflags - super block flags that track conversion in progress
    (like UUID or checksums)

Other notable changes or fixes:

- space cache v1 marked as deprecated (a warning printed in syslog), the
  free-space tree (i.e. the v2) has been default in "mkfs.btrfs" since 5.15,
  the kernel code will be removed in the future on a conservative schedule

- tree checker improvements:
  - validate data reference items
  - validate directory item type

- send also detects last extent suitable for cloning (and not a write)

- extent map shrinker (a memory reclaim optimization) added in 6.10 now
  available only under CONFIG_BTRFS_DEBUG due to performance problems

- update target inode's ctime on unlink,
  `mandated by POSIX <https://pubs.opengroup.org/onlinepubs/9699919799/functions/unlink.html>`__

- in zoned mode, detect unexpected zone write pointer change

6.12 (Nov 2024)
---------------

Pull requests:
`v6.12-rc1 <https://git.kernel.org/linus/7a40974fd0efa3698de4c6d1d0ee0436bcc4445d>`__,
`v6.12-rc1 <https://git.kernel.org/linus/a1fb2fcbb60650621a7e3238629a8bfb94147b8e>`__,
`v6.12-rc2 <https://git.kernel.org/linus/79eb2c07afbe4d165734ea61a258dd8410ec6624>`__,
`v6.12-rc3 <https://git.kernel.org/linus/eb952c47d154ba2aac794b99c66c3c45eb4cc4ec>`__,
`v6.12-rc4 <https://git.kernel.org/linus/667b1d41b25b9b6b19c8af9d673ccb93b451b527>`__,
`v6.12-rc5 <https://git.kernel.org/linus/4e46774408d942efe4eb35dc62e5af3af71b9a30>`__,
`v6.12-rc6 <https://git.kernel.org/linus/6b4926494ed872803bb0b3c59440ac25c35c9869>`__,
`v6.12-rc7 <https://git.kernel.org/linus/9183e033ec4f8bdac778070ebccdd41727da2305>`__,
`v6.12 <https://git.kernel.org/linus/c9dd4571ad38654f26c07ff2b7c7dba03301fc76>`__

User visible changes:

- the FSTRIM ioctl updates the processed range even after an error or interruption

- cleaner thread is woken up in SYNC ioctl instead of waking the transaction
  thread that can take some delay before waking up the cleaner, this can speed
  up cleaning of deleted subvolumes

- print an error message when opening a device fail, e.g. when it's unexpectedly read-only

Core changes:

- improved extent map handling in various ways (locking, iteration, ...)
- new assertions and locking annotations
- raid-stripe-tree locking fixes
- use xarray for tracking dirty qgroup extents, switched from rb-tree
- turn the subpage test to compile-time condition if possible (e.g.  on x86_64
  with 4K pages), this allows to skip a lot of ifs and remove dead code
- more preparatory work for compression in subpage mode

Cleanups and refactoring:

- folio API conversions, many simple cases where page is passed so switch it to
  folios
- more subpage code refactoring, update page state bitmap processing
- introduce auto free for btrfs_path structure, use for the simple cases

6.13 (Jan 2025)
---------------

Pull requests:
`v6.13-rc1 <https://git.kernel.org/linus/c14a8a4c04c5859322eb5801db662b56b2294f67>`__,
`v6.13-rc2 <https://git.kernel.org/linus/feffde684ac29a3b7aec82d2df850fbdbdee55e4>`__,
`v6.13-rc3 <https://git.kernel.org/linus/5a087a6b17eeb64893b81d08d38e6f6300419ee5>`__,
`v6.13-rc4 <https://git.kernel.org/linus/eabcdba3ad4098460a376538df2ae36500223c1e>`__,
`v6.13-rc5 <https://git.kernel.org/linus/c059361673e487fe33bb736fb944f313024ad726>`__,
`v6.13-rc7 <https://git.kernel.org/linus/643e2e259c2b25a2af0ae4c23c6e16586d9fd19c>`__,
`v6.13 <https://git.kernel.org/linus/ed8fd8d5dd4aa250e18152b80cbac24de7335488>`__

User visible changes:

- wire encoded read (ioctl) to io_uring commands, this can be used on itself,
  in the future this will allow 'send' to be asynchronous. As a consequence,
  the encoded read ioctl can also work in non-blocking mode

- new ioctl to wait for cleaned subvolumes, no need to use the generic and
  root-only SEARCH_TREE ioctl, will be used by "btrfs subvol sync"

- recognize different paths/symlinks for the same devices and don't report them
  during rescanning, this can be observed with LVM or DM

- seeding device use case change, the sprout device (the one capturing new
  writes) will not clear the read-only status of the super block; this prevents
  accumulating space from deleted snapshots

- swapfile activation updates that are nice to CPU and activation is interruptible

Performance improvements:

- reduce lock contention when traversing extent buffers
- reduce extent tree lock contention when searching for inline backref
- switch from rb-trees to xarray for delayed ref tracking, improvements due to
  better cache locality, branching factors and more compact data structures
- enable extent map shrinker again (prevent memory exhaustion under some types
  of IO load), reworked to run in a single worker thread (there used to be
  problems causing long stalls under memory pressure)

Core changes:

- raid-stripe-tree feature updates:

  - make device replace and scrub work
  - implement partial deletion of stripe extents
  - new selftests

- split the config option BTRFS_DEBUG and add EXPERIMENTAL for
  features that are experimental or with known problems so we don't
  misuse debugging config for that

- subpage mode updates (sector < page):

  - update compression implementations
  - update writepage, writeback

- continued folio API conversions, buffered writes
- make buffered write copy one page at a time, preparatory work for
  future integration with large folios, may cause performance drop
- proper locking of root item regarding starting send
- error handling improvements

- code cleanups and refactoring:

  - dead code removal
  - unused parameter reduction
  - lockdep assertions

6.14 (Mar 2025)
---------------

Pull requests:
`v6.14-rc1 <https://git.kernel.org/linus/0eb4aaa230d725fa9b1cd758c0f17abca5597af6>`__,
`v6.14-rc2 <https://git.kernel.org/linus/92514ef226f511f2ca1fb1b8752966097518edc0>`__,
`v6.14-rc3 <https://git.kernel.org/linus/945ce413ac14388219afe09de84ee08994f05e53>`__,
`v6.14-rc5 <https://git.kernel.org/linus/cc8a0934d099b8153fc880a3588eec4791a7bccb>`__,
`v6.14-rc6 <https://git.kernel.org/linus/6ceb6346b0436ea6591c33ab6ab22e5077ed17e7>`__,

User visible changes, features:

- rebuilding of the free space tree at mount time is done in more transactions,
  fix potential hangs when the transaction thread is blocked due to large
  amount of block groups

- more read IO balancing strategies (experimental config), add two new ways how
  to select a device for read if the profiles allow that (all RAID1*), the
  current default selects the device by pid which is good on average but less
  performant for single reader workloads

  - select preferred device for all reads (namely for testing)
  - round-robin, balance reads across devices relevant for the requested IO range

  - add encoded write ioctl support to io_uring (read was added in
    6.12), basis for writing send stream using that instead of
    syscalls, non-blocking mode is not yet implemented

  - support FS_IOC_READ_VERITY_METADATA, applications can use the
    metadata to do their own verification

  - pass inode's i_write_hint to bios, for parity with other
    filesystems, ioctls F_GET_RW_HINT/F_SET_RW_HINT

Core:

- in zoned mode: allow to directly reclaim a block group by simply
  resetting it, then it can be reused and another block group does
  not need to be allocated

- super block validation now also does more comprehensive sys array
  validation, adding it to the points where superblock is validated
  (post-read, pre-write)

- subpage mode fixes:

  - fix double accounting of blocks due to some races
  - improved or fixed error handling in a few cases (compression,
    delalloc)

- raid stripe tree:

  - fix various cases with extent range splitting or deleting
  - implement hole punching to extent range
  - reduce number of stripe tree lookups during bio submission
  - more self-tests

- updated self-tests (delayed refs)

- error handling improvements

- cleanups, refactoring

  - remove rest of backref caching infrastructure from relocation,
    not needed anymore
  - error message updates
  - remove unnecessary calls when extent buffer was marked dirty
  - unused parameter removal
  - code moved to new files

6.15 (May 2025)
---------------

Pull requests:
`v6.15-rc1 <https://git.kernel.org/linus/fd71def6d9abc5ae362fb9995d46049b7b0ed391>`__,
`v6.15-rc3 <https://git.kernel.org/linus/0cb9ce06a682b251d350ded18965a3dfa5d13595>`__,
`v6.15-rc4 <https://git.kernel.org/linus/bc3372351d0c8b2726b7d4229b878342e3e6b0e8>`__,
`v6.15-rc5 <https://git.kernel.org/linus/7a13c14ee59d4f6c5f4277a86516cbc73a1383a8>`__,
`v6.15-rc6 <https://git.kernel.org/linus/0d8d44db295ccad20052d6301ef49ff01fb8ae2d>`__,
`v6.15-rc7 <https://git.kernel.org/linus/74a6325597464e940a33e56e98f6899ef77728d8>`__,

User visible changes:

-  fall back to buffered write if direct io is done on a file that requires checksums

   -  this avoids a problem with checksum mismatch errors, observed e.g. on
      virtual images when writes to pages under writeback cause the checksum
      mismatch reports

   -  this may lead to some performance degradation but currently the
      recommended setup for VM images is to use the NOCOW file attribute that
      also disables checksums

-  fast/realtime zstd levels -15 to -1

   - supported by mount options (compress=zstd:-5) and defrag ioctl
   - improved speed, reduced compression ratio, check the `commit for sample
     measurements <https://git.kernel.org/linus/da798fa519df6f995a493ca5105c72ccc4fc7b75>`__.

-  defrag ioctl extended to accept negative compression levels

-  subpage mode

   -  remove warning when subpage mode is used, the feature is now reasonably
      complete and tested
   -  in debug mode allow to create 2K b-tree nodes to allow testing subpage on
      x86_64 with 4K pages too

-  fixes

   -  escape subvolume path in mount option list so it cannot be wrongly parsed
      when the path contains ","

   -  reinstate message when setting a large value of mount option 'commit'

Performance improvements:

-  in send, better file path caching improves runtime (on sample load by -30%)

-  on s390x with hardware zlib support prepare the input buffer in a better way
   to get the best results from the acceleration

-  minor speed improvement in encoded read, avoid memory allocation in
   synchronous mode

Core:

- enable stable writes on inodes, replacing manually waiting for writeback and
  allowing to skip that on inodes without checksums

- add last checks and warnings for out-of-band dirty writes to pages, requiring
  a fixup ("fixup worker"), this should not be necessary since 5.8 where
  get_user_page() and pin_user_pages*() prevent this

- more preparations for large folio support

6.16 (Jul 2025)
---------------

`v6.16-rc1 <https://git.kernel.org/linus/5e82ed5ca4b510e0ff53af1e12e94e6aa1fe5a93>`__,
`v6.16-rc1 <https://git.kernel.org/linus/a56baa225308e697163e74bae0cc623a294073d4>`__,
`v6.16-rc4 <https://git.kernel.org/linus/5ca7fe213ba3113dde19c4cd46347c16d9e69f81>`__,
`v6.16-rc5 <https://git.kernel.org/linus/4c06e63b92038fadb566b652ec3ec04e228931e8>`__,

Performance:

- extent buffer conversion to xarray gains throughput and runtime improvements
  on metadata heavy operations doing writeback (sample test shows +50%
  throughput, -33% runtime)

- extent io tree cleanups lead to performance improvements by avoiding
  unnecessary searches or repeated searches

- more efficient extent unpinning when committing transaction (estimated run
  time improvement 3-5%)

User visible changes:

- remove standalone mount option 'nologreplay', deprecated in 5.9, replacement
  is 'rescue=nologreplay'

- in scrub, update reporting, add back device stats message after detected
  errors (accidentally removed during recent refactoring)

Core:

- convert extent buffer radix tree to xarray

- in subpage mode, move block perfect compression out of experimental build

- in zoned mode, introduce sub block groups to allow managing special block
  groups, like the one for relocation or tree-log, to handle some corner cases
  of ENOSPC

- continued preparations for large folios: add support where missing:
  compression, buffered write, defrag, hole punching, subpage, send

- fix fsync of files with no hard links not persisting deletion

- reject tree blocks which are not nodesize aligned, a precaution from 4.9
  times

- enhanced ASSERT() macro with optional format strings

5.x
---

5.0 (Mar 2019)
^^^^^^^^^^^^^^

Pull requests:
`v5.0-rc1 <https://git.kernel.org/linus/32ee34eddad13cd44ad0cb3e659fe6fd49143b62>`__,
`v5.0-rc2 <https://git.kernel.org/linus/6b529fb0a3eabf9c4cc3e94c11477250379ce6d8>`__,
`v5.0-rc3 <https://git.kernel.org/linus/1be969f4682b0aa1995e46fba51502de55f15ce8>`__,
`v5.0-rc5 <https://git.kernel.org/linus/312b3a93dda6db9354b0c6b0f1868c1434e8c787>`__

Features, highlights:

- swapfile support (with some limitations)
- metadata uuid - new feature that allows fast uuid change without rewriting all metadata blocks (backward incompatible)
- balance messages in the syslog when operations start or stop

Fixes:

- improved check of filesystem id associated with a device during scan to
  detect duplicate devices that could be mixed up during mount
- fix device replace state transitions
- fix a crash due to a race when quotas are enabled during snapshot creation
- GFP_NOFS/memalloc_nofs_* fixes
- fsync fixes

Other:

- remove first phase of balance that tried to remove some space (not necessary)
- separate reserve for delayed refs from global reserve
- cleanups
- see [https://git.kernel.org/linus/32ee34eddad13cd44ad0cb3e659fe6fd49143b62 pull request]

5.1 (May 2019)
^^^^^^^^^^^^^^

Pull requests:
`v5.1-rc1 <https://git.kernel.org/linus/b1e243957e9b3ba8e820fb8583bdf18e7c737aa2>`__,
`v5.1-rc1 <https://git.kernel.org/linus/92825b0298ca6822085ef483f914b6e0dea9bf66>`__,
`v5.1-rc3 <https://git.kernel.org/linus/65ae689329c5d6a149b9201df9321368fbdb6a5c>`__,
`v5.1-rc5 <https://git.kernel.org/linus/2d06b235815e6bd20395f3db9ada786a6f7a876e>`__,
`v5.1-rc7 <https://git.kernel.org/linus/d0473f978e61557464daa8547008fa2cd0c63a17>`__

New features, highlights:

- zstd compression levels can be set as mount options
- new ioctl to unregister scanned devices
- scrub prints messages about start/stop/cancel to the log

Other changes:

- qgroups skip some work (est. speedup during balance 20%)
- reclaim vs GFP_KERNEL fixes
- fsync fixes for rename/unlink/rmdir
- improved enospc handling on a highly fragmented filesystem
- no trim on filesystem with unreplayed log
- see [https://git.kernel.org/linus/b1e243957e9b3ba8e820fb8583bdf18e7c737aa2 pull request]

5.2 (Jul 2019)
^^^^^^^^^^^^^^

Pull requests:
`v5.2-rc1 <https://git.kernel.org/linus/9f2e3a53f7ec9ef55e9d01bc29a6285d291c151e>`__,
`v5.2-rc2 <https://git.kernel.org/linus/f49aa1de98363b6c5fba4637678d6b0ba3d18065>`__,
`v5.2-rc3 <https://git.kernel.org/linus/318adf8e4bfdcb0bce1833824564b1f24278927b>`__,
`v5.2-rc5 <https://git.kernel.org/linus/6fa425a2651515f8d262f2c1d972c6632e7c941d>`__,
`v5.2-rc6 <https://git.kernel.org/linus/bed3c0d84e7e25c8e0964d297794f4c215b01f33>`__

User visible changes, highlights:

- better read time and write checks to catch errors early and before writing data to disk
- qgroups + metadata relocation: last speed up patch in the series there should
  be no overhead comparing balance with and without qgroups
- FIEMAP ioctl does not start a transaction unnecessarily
- LOGICAL_INO (v1, v2) does not start transaction unnecessarily
- fsync on files with many (but not too many) hardlinks is faster
- send tries harder to find ranges to clone
- trim/discard will skip unallocated chunks that haven't been touched since the last mount
- tree-checker does more validations: device item, inode item, block group item:
- improved space flushing logic for intense DIO vs buffered workloads
- metadata reservations for delalloc reworked to better adapt in many-writers/low-space scenarios

Fixes:

- send flushes delayed allocation before start
- fix fallocate with qgroups accounting underflow
- send and dedupe can't be run at the same time
- fix crash in relocation/balance after resume

Other:

- new tracepoints for locking
- async write preallocates memory to avoid failures deep in call chains
- lots of cleanups

5.3 (Sep 2019)
^^^^^^^^^^^^^^

Pull requests:
`v5.3-rc1 <https://git.kernel.org/linus/a18f8775419d3df282dd83efdb51c5a64d092f31>`__,
`v5.3-rc2 <https://git.kernel.org/linus/21c730d7347126886c40453feb973161f4ae3fb3>`__,
`v5.3-rc2 <https://git.kernel.org/linus/4792ba1f1ff0db30369f7016c1611fda3f84b895>`__,
`v5.3-rc3 <https://git.kernel.org/linus/d38c3fa6f959b8b5b167f120d70d66418714dbe4>`__,
`v5.3-rc5 <https://git.kernel.org/linus/3039fadf2bfdc104dc963820c305778c7c1a6229>`__,
`v5.3 <https://git.kernel.org/linus/1b304a1ae45de4df7d773f0a39d1100aabca615b>`__

New features, highlights:

- chunks that have been trimmed and unchanged since last mount are tracked and skipped on repeated trims
- use hw assisted crc32c on more arches
- the RAID56 incompat bit is automatically removed when the last block group of that type is removed

Fixes:

- update ctime/mtime/iversion after hole punching
- fsync fixes
- send and balance can't be run at the same time

Other:

- code refactoring, file splits
- preparatory work for more checksums
- tree checker to verify lengths of various items
- delayed iput happens at unlink time, not in cleaner thread
- new tracepoints for space updates

5.4 (Nov 2019)
^^^^^^^^^^^^^^

Pull requests:
`v5.4-rc1 <https://git.kernel.org/linus/7d14df2d280fb7411eba2eb96682da0683ad97f6>`__,
`v5.4-rc1 <https://git.kernel.org/linus/bb48a59135926ece9b1361e8b96b33fc658830bc>`__,
`v5.4-rc3 <https://git.kernel.org/linus/f8779876d4a79d243870a5b5d60009e4ec6f22f4>`__,
`v5.4-rc5 <https://git.kernel.org/linus/54955e3bfde54dcdd29694741f2ddfc6b763b193>`__,
`v5.4-rc7 <https://git.kernel.org/linus/00aff6836241ae5654895dcea10e6d4fc5878ca6>`__,
`v5.4-rc8 <https://git.kernel.org/linus/afd7a71872f14062cc12cac126bb8e219e7dacf6>`__

- tree checker: added sanity checks for tree items, extent items, and references
- deprecated subvolume creation mode BTRFS_SUBVOL_CREATE_ASYNC
- qgroup relation deletion tries harder, orphan entries are removed too
- space handling improvements (ticket reservations, flushing, overcommit logic)
- fix possible lockups during send of large subvolumes
- see [https://git.kernel.org/linus/7d14df2d280fb7411eba2eb96682da0683ad97f6 pull request]

5.5 (Jan 2020)
^^^^^^^^^^^^^^

Pull requests:
`v5.5-rc1 <https://git.kernel.org/linus/97d0bf96a0d0986f466c3ff59f2ace801e33dc69>`__,
`v5.5-rc1 <https://git.kernel.org/linus/ae36607b669eb28791b02097a87d3d2e1589e88f>`__,
`v5.5-rc2 <https://git.kernel.org/linus/6794862a16ef41f753abd75c03a152836e4c8028>`__,
`v5.5-rc3 <https://git.kernel.org/linus/2187f215ebaac73ddbd814696d7c7fa34f0c3de0>`__,
`v5.5-rc5 <https://git.kernel.org/linus/3a562aee727a7bfbb3a37b1aa934118397dad701>`__,
`v5.5-rc7 <https://git.kernel.org/linus/effaf90137e3a9bb9702746f993f369a53c4185f>`__,
`v5.5 <https://git.kernel.org/linus/a075f23dd4b036ebaf918b3af477aa1f249ddfa0>`__

- new block group profiles: RAID1 with 3- and 4- copies

  - RAID1 in btrfs has always 2 copies, now add support for 3 and 4
  - this is an incompat feature (named RAID1C34)
  - recommended use of RAID1C3 is replacement of RAID6 profile on metadata,
    this brings a more reliable resiliency against 2 device loss/damage

- support for new checksums

  - per-filesystem, set at mkfs time
  - fast hash (crc32c successor): xxhash, 64bit digest
  - strong hashes (both 256bit): sha256 (slower, FIPS), blake2b (faster)

- speed up lseek, don't take inode locks unnecessarily, this can speed up parallel SEEK_CUR/SEEK_SET/SEEK_END by 80%
- send:

  - allow clone operations within the same file
  - limit maximum number of sent clone references to avoid slow backref walking

- error message improvements: device scan prints process name and PID
- new tree-checker sanity tests (INODE_ITEM, DIR_ITEM, DIR_INDEX, INODE_REF, XATTR)

5.6 (Mar 2020)
^^^^^^^^^^^^^^

Pull requests:
`v5.6-rc1 <https://git.kernel.org/linus/81a046b18b331ed6192e6fd9ff6d12a1f18058cf>`__,
`v5.6-rc1 <https://git.kernel.org/linus/b5f7ab6b1c4ed967fb76258f79251193cb1ad41d>`__,
`v5.6-rc1 <https://git.kernel.org/linus/ad801428366ebbd541a5b8a1bf4d8b57ee7a8200>`__,
`v5.6-rc2 <https://git.kernel.org/linus/713db356041071d16360e82247de3107ec9ed57f>`__,
`v5.6-rc3 <https://git.kernel.org/linus/eaea2947063ac694cddff1787d43e7807490dbc7>`__,
`v5.6-rc3 <https://git.kernel.org/linus/d2eee25858f246051b49c42c411629c78513e2a8>`__,
`v5.6-rc5 <https://git.kernel.org/linus/30fe0d07fd7b27d41d9b31a224052cc4e910947a>`__,
`v5.6-rc7 <https://git.kernel.org/linus/67d584e33e54c3f33c8541928aa7115388c97433>`__

Highlights:

- async discard

  - "mount -o discard=async" to enable it
  - freed extents are not discarded immediately, but grouped together and
    trimmed later, with IO rate limiting
  - the actual discard IO requests have been moved out of transaction commit
    to a worker thread, improving commit latency
  - IO rate and request size can be tuned by sysfs files, for now enabled only
    with CONFIG_BTRFS_DEBUG as we might need to add/delete the files and don't
    have a stable-ish ABI for general use, defaults are conservative

- export device state info in sysfs, e.g. missing, writeable
- no discard of extents known to be untouched on disk (e.g. after reservation)
- device stats reset is logged with process name and PID that called the ioctl

Core changes:

- qgroup assign returns ENOTCONN when quotas not enabled, used to return EINVAL
  that was confusing
- device closing does not need to allocate memory anymore
- snapshot aware code got removed, disabled for years due to performance
  problems, reimplementation will allow to select whether defrag breaks or does
  not break COW on shared extents
- tree-checker:

  - check leaf chunk item size, cross check against number of stripes
  - verify location keys for DIR_ITEM, DIR_INDEX and XATTR items
  - new self test for physical -> logical mapping code, used for super block range exclusion

Fixes:

- fix missing hole after hole punching and fsync when using NO_HOLES
- writeback: range cyclic mode could miss some dirty pages and lead to OOM
- two more corner cases for metadata_uuid change after power loss during the change
- fix infinite loop during fsync after mix of rename operations

5.7 (May 2020)
^^^^^^^^^^^^^^

Pull requests:
`v5.7-rc1 <https://git.kernel.org/linus/15c981d16d70e8a5be297fa4af07a64ab7e080ed>`__,
`v5.7-rc2 <https://git.kernel.org/linus/6cc9306b8fc03019e81e4f10c93ff0528cba5217>`__,
`v5.7-rc2 <https://git.kernel.org/linus/c5304dd59b0c26cd9744121b77ca61f014929ba8>`__,
`v5.7-rc4 <https://git.kernel.org/linus/51184ae37e0518fd90cb437a2fbc953ae558cd0d>`__,
`v5.7-rc4 <https://git.kernel.org/linus/262f7a6b8317a06e7d51befb690f0bca06a473ea>`__

Highlights:

- v2 of ioctl to delete subvolumes, allowing to delete by id and more future extensions
- removal of obsolete ioctl flag BTRFS_SUBVOL_CREATE_ASYNC
- more responsive balance cancel
- speedup of extent back reference resolution
- reflink/clone_range works on inline extents
- lots of other core changes, see the [https://git.kernel.org/linus/15c981d16d70e8a5be297fa4af07a64ab7e080ed pull request]

5.8 (Aug 2020)
^^^^^^^^^^^^^^

Pull requests:
`v5.8-rc1 <https://git.kernel.org/linus/f3cdc8ae116e27d84e1f33c7a2995960cebb73ac>`__,
`v5.8-rc1 <https://git.kernel.org/linus/9d645db853a4cd1b7077931491d0055602d3d420>`__,
`v5.8-rc3 <https://git.kernel.org/linus/3e08a95294a4fb3702bb3d35ed08028433c37fe6>`__,
`v5.8-rc5 <https://git.kernel.org/linus/aa27b32b76d0b1b242d43977da0e5358da1c825f>`__,
`v5.8-rc5 <https://git.kernel.org/linus/72c34e8d7099c329c2934c2ac9c886f638b6edaf>`__,
`v5.8-rc7 <https://git.kernel.org/linus/0669704270e142483d80cfda5c526426c1a89711>`__

Highlights:

- speedup dead root detection during orphan cleanup
- send will emit file capabilities after chown

Core changes:

- improved global block reserve utilization
- direct io cleanups and fixes
- refactored block group reading code

5.9 (Oct 2020)
^^^^^^^^^^^^^^

Pull requests:
`v5.9-rc1 <https://git.kernel.org/linus/6dec9f406c1f2de6d750de0fc9d19872d9c4bf0d>`__,
`v5.9-rc1 <https://git.kernel.org/linus/23c2c8c6fa325939f95d840f54bfdec3cb76906c>`__,
`v5.9-rc3 <https://git.kernel.org/linus/9907ab371426da8b3cffa6cc3e4ae54829559207>`__,
`v5.9-rc4 <https://git.kernel.org/linus/dcdfd9cc28ddd356d24d5461119e4c1d19284ff5>`__,
`v5.9-rc4 <https://git.kernel.org/linus/26acd8b07a07000d9f61ee64dc6fde0494997b47>`__,
`v5.9-rc5 <https://git.kernel.org/linus/edf6b0e1e4ddb12e022ce0c17829bad6d4161ea7>`__,
`v5.9-rc6 <https://git.kernel.org/linus/fc4f28bb3daf3265d6bc5f73b497306985bb23ab>`__,
`v5.9-rc7 <https://git.kernel.org/linus/bffac4b5435a07bf26604385ae533adff3cccf23>`__,
`v5.9-rc8 <https://git.kernel.org/linus/4e3b9ce271b4b54d2293a3916d22e4ddc0c89aab>`__

Highlights:

- add mount option ''rescue'' to unify options for various recovery tasks on a mounted filesystems
- mount option ''inode_cache'' is deprecated and will be removed in 5.11
- removed deprecated options ''alloc_start'' and ''subvolrootid''
- sysfs exports information about qgroups and relations
- FS_INFO ioctl exports more information from the filesystem (notably type of checksum)
- running balance detects Ctrl-C too
- performance improvements in fsync
- mount-time prefetch of chunk tree

5.10 (Dec 2020)
^^^^^^^^^^^^^^^

Pull requests:
`v5.10-rc1 <https://git.kernel.org/linus/11e3235b4399f7e626caa791a68a0ea8337f6683>`__,
`v5.10-rc2 <https://git.kernel.org/linus/f5d808567a51d97e171e0a8111813f973bf4ac12>`__,
`v5.10-rc4 <https://git.kernel.org/linus/e2f0c565ec70eb9e4d3b98deb5892af62de8b98d>`__,
`v5.10-rc6 <https://git.kernel.org/linus/a17a3ca55e96d20e25e8b1a7cd08192ce2bac3cc>`__

Highlights:

- performance improvements in fsync (dbench workload: higher throughput, lower latency)
- sysfs exports current exclusive operation (balance, resize, device add/del/...)
- sysfs exports supported send stream version

Core:

- direct io uses iomap infrastructure (no more ''struct buffer_head'')
- space reservations for data now use ticket infrastructure
- cleanups, refactoring, preparatory work
- error handling improvements
- fixes

5.11 (Feb 2021)
^^^^^^^^^^^^^^^

Pull requests:
`v5.11-rc1 <https://git.kernel.org/linus/f1ee3b8829006b3fda999f00f0059aa327e3f3d0>`__,
`v5.11-rc3 <https://git.kernel.org/linus/71c061d2443814de15e177489d5cc00a4a253ef3>`__,
`v5.11-rc4 <https://git.kernel.org/linus/6e68b9961ff690ace07fac22c3c7752882ecc40a>`__,
`v5.11-rc5 <https://git.kernel.org/linus/9791581c049c10929e97098374dd1716a81fefcc>`__,
`v5.11-rc6 <https://git.kernel.org/linus/c05d51c773fb365bdbd683b3e4e80679c8b8b176>`__,
`v5.11 <https://git.kernel.org/linus/e42ee56fe59759023cb252fabb3d6f279fe8cec8>`__

- new mount option ''rescue'', various modes how to access a damaged filesystem
- sysfs updates: filesystem generation, supported ''rescue'' modes, read mirror policy
- removed feature: ''mount -o inode_cache''
- free space tree fixes, v1 cache removed during conversion

Core:

- locking switched to standard rw semaphores
- direct IO ported to iomap infrastructure
- zoned allocation mode preparation
- subpage blocksize preparation
- various performance improvements (skipping unnecessary work)

5.12 (Apr 2021)
^^^^^^^^^^^^^^^

Pull requests:
`v5.12-rc1 <https://git.kernel.org/linus/f9d58de23152f2c16f326d7e014cfa2933b00304>`__,
`v5.12-rc1 <https://git.kernel.org/linus/6f3952cbe00b74739f540981d1afe84cd4dac879>`__,
`v5.12-rc2 <https://git.kernel.org/linus/c608aca57dd034d09f307b109b670d1cfb829279>`__,
`v5.12-rc2 <https://git.kernel.org/linus/7a7fd0de4a9804299793e564a555a49c1fc924cb>`__,
`v5.12-rc2 <https://git.kernel.org/linus/f09b04cc6447331e731629e8b72587287f3a4490>`__,
`v5.12-rc4 <https://git.kernel.org/linus/81aa0968b7ea6dbabcdcda37dc8434dca6e1565b>`__,
`v5.12-rc5 <https://git.kernel.org/linus/701c09c988bd60d950d49c48993b6c06efbfba7f>`__,
`v5.12-rc7 <https://git.kernel.org/linus/7d900724913cb293620a05c5a3134710db95d0d9>`__

Features:

- zoned mode (SMR/ZBC/ZNS friendly allocation mode), first working version with limitations
- misc performance improvements

  - flushing and ticket space reservations
  - preemptive background flushing
  - less lock contention for delayed refs
  - dbench-like workload (+7% throughput, -20% latency)

Core changes:

- subpage block size support preparations

Fixes:

- swapfile fixes (vs scrub, activation vs snapshot creation)

5.13 (Jun 2021)
^^^^^^^^^^^^^^^

Pull requests:
`v5.13-rc1 <https://git.kernel.org/linus/55ba0fe059a577fa08f23223991b24564962620f>`__,
`v5.13-rc2 <https://git.kernel.org/linus/142b507f911c5a502dbb8f603216cb0ea8a79a48>`__,
`v5.13-rc2 <https://git.kernel.org/linus/88b06399c9c766c283e070b022b5ceafa4f63f19>`__,
`v5.13-rc3 <https://git.kernel.org/linus/8ac91e6c6033ebc12c5c1e4aa171b81a662bd70f>`__,
`v5.13-rc3 <https://git.kernel.org/linus/45af60e7ced07ae3def41368c3d260dbf496fbce>`__,
`v5.13-rc5 <https://git.kernel.org/linus/fd2ff2774e90a0ba58f1158d7ea095af51f31644>`__,
`v5.13-rc6 <https://git.kernel.org/linus/cc6cf827dd6858966cb5086703447cb68186650e>`__,
`v5.13-rc7 <https://git.kernel.org/linus/6fab154a33ba9b3574ba74a86ed085e0ed8454cb>`__

User visible improvements

- readahead for send, improving run time of full send by 10% and for incremental by 25%
- make reflinks respect O_SYNC, O_DSYNC and S_SYNC flags
- export supported sectorsize values in sysfs (currently only page size, more
  once full subpage support lands)
- more graceful errors and warnings on 32bit systems when logical addresses for
  metadata reach the limit posed by unsigned long in page::index

  - error: fail mount if there's a metadata block beyond the limit
  - error: new metadata block would be at unreachable address
  - warn when 5/8th of the limit is reached, for 4K page systems it's 10T, for 64K page it's 160T

- zoned mode

  - relocated zones get reset at the end instead of discard
  - automatic background reclaim of zones that have 75%+ of unusable space, the
    threshold is tunable in sysfs

Fixes

- fix inefficient preemptive reclaim calculations
- fix exhaustion of the system chunk array due to concurrent allocations
- fix fallback to no compression when racing with remount
- fix unmountable seed device after fstrim
- fix fiemap to print extents that could get misreported due to internal extent
  splitting and logical merging for fiemap output
- preemptive fix for dm-crypt on zoned device that does not properly advertise zoned support

Core changes

- add inode lock to synchronize mmap and other block updates (e.g. deduplication, fallocate, fsync)
- subpage support update: metadata changes now support read and write
- error handling through out relocation call paths
- many other cleanups and code simplifications

5.14 (Aug 2021)
^^^^^^^^^^^^^^^

Pull requests:
`v5.14-rc1 <https://git.kernel.org/linus/122fa8c588316aacafe7e5a393bb3e875eaf5b25>`__,
`v5.14-rc2 <https://git.kernel.org/linus/f02bf8578bd8dd400903291ccebc69665adc911c>`__,
`v5.14-rc3 <https://git.kernel.org/linus/f0fddcec6b6254b4b3611388786bbafb703ad257>`__,
`v5.14-rc4 <https://git.kernel.org/linus/051df241e44693dba8f4e1e74184237f55dd811d>`__,
`v5.14-rc7 <https://git.kernel.org/linus/d6d09a6942050f21b065a134169002b4d6b701ef>`__,
`v5.14 <https://git.kernel.org/linus/9b49ceb8545b8eca68c03388a07ecca7caa5d9c1>`__

Highlights:

- new sysfs knob to limit scrub IO bandwidth per device
- device stats are also available in /sys/fs/btrfs/FSID/devinfo/DEVID/error_stats
- support cancellable resize and device delete ioctls
- change how the empty value is interpreted when setting a property, so far we
  have only 'btrfs.compression' and we need to distinguish a reset to defaults
  and setting "do not compress", in general the empty value will always mean
  'reset to defaults' for any other property, for compression it's either 'no'
  or 'none' to forbid compression
- performance improvements (xattrs, truncate)
- space handling improvements, preemptive flushing
- more subpage support preparation

5.15 (Nov 2021)
^^^^^^^^^^^^^^^

Pull requests:
`v5.15-rc1 <https://git.kernel.org/linus/87045e6546078dae215d1bd3b2bc82b3ada3ca77>`__,
`v5.15-rc1 <https://git.kernel.org/linus/8dde20867c443aedf6d64d8a494e8703d7ba53cb>`__,
`v5.15-rc3 <https://git.kernel.org/linus/f9e36107ec70445fbdc2562ba5b60c0a7ed57c20>`__,
`v5.15-rc6 <https://git.kernel.org/linus/1986c10acc9c906e453fb19d86e6342e8e525824>`__,
`v5.15 <https://git.kernel.org/linus/fd919bbd334f22486ee2e9c16ceefe833bb9e32f>`__

Features:

- fs-verity support, using standard ioctls, backward compatible with read-only
  limitation on inodes with previously enabled fs-verity
- idmapped mount support
- make mount with rescue=ibadroots more tolerant to partially damaged trees
- allow raid0 on a single device and raid10 on two devices, degenerate cases
  but might be useful as an intermediate step during conversion to other
  profiles
- zoned mode block group auto reclaim can be disabled via sysfs knob

Performance improvements:

- continue readahead of node siblings even if target node is in memory, could speed up full send (on sample test +11%)
- batching of delayed items can speed up creating many files
- fsync/tree-log speedups

  - avoid unnecessary work (gains +2% throughput, -2% run time on sample load)
  - reduced lock contention on renames (on dbench +4% throughput, up to -30% latency)

Fixes:

- various zoned mode fixes
- preemptive flushing threshold tuning, avoid excessive work on almost full filesystems

Core:

- continued subpage support, preparation for implementing remaining features
  like compression and defragmentation; with some limitations, write is now
  enabled on 64K page systems with 4K sectors, still considered experimental

  - no readahead on compressed reads
  - inline extents disabled
  - disabled raid56 profile conversion and mount

- improved flushing logic, fixing early ENOSPC on some workloads
- inode flags have been internally split to read-only and read-write incompat bit parts, used by fs-verity
- new tree items for fs-verity: descriptor item, Merkle tree item
- inode operations extended to be namespace-aware
- cleanups and refactoring

5.16 (Jan 2022)
^^^^^^^^^^^^^^^

Pull requests:
`v5.16-rc1 <https://git.kernel.org/linus/037c50bfbeb33b4c74e120eef5b8b99d8f025418>`__,
`v5.16-rc1 <https://git.kernel.org/linus/6070dcc8e5b1495e11ffd467c77eaeac40f95a93>`__,
`v5.16-rc2 <https://git.kernel.org/linus/6fdf886424cf8c4fff96a20189c00606327e5df6>`__,
`v5.16-rc3 <https://git.kernel.org/linus/7e63545264c3d1844189e47ac8a4dabc03e11d8b>`__,
`v5.16-rc5 <https://git.kernel.org/linus/6f513529296fd4f696afb4354c46508abe646541>`__,
`v5.16-rc6 <https://git.kernel.org/linus/9609134186b710fa2104ac153bcc27b11c3e8c21>`__

Related projects: kernel port of zstd 1.4.10 also
[https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=c8c109546a19613d323a319d0c921cb1f317e629
released] in 5.16

Performance related:

- misc small inode logging improvements (+3% throughput, -11% latency on sample dbench workload)
- more efficient directory logging: bulk item insertion, less tree searches and locking
- speed up bulk insertion of items into a b-tree, which is used when logging
  directories, when running delayed items for directories (fsync and
  transaction commits) and when running the slow path (full sync) of an fsync
  (bulk creation run time -4%, deletion -12%)

Core:

- continued subpage support

  - make defragmentation work
  - make compression write work

- zoned mode

  - support ZNS (zoned namespaces), zone capacity is number of usable blocks in each zone
  - add dedicated block group (zoned) for relocation, to prevent out of order writes in some cases
  - greedy block group reclaim, pick the ones with least usable space first

- preparatory work for send protocol updates
- error handling improvements
- cleanups and refactoring

5.17 (Mar 2022)
^^^^^^^^^^^^^^^

Pull requests:
`v5.17-rc1 <https://git.kernel.org/linus/d601e58c5f2901783428bc1181e83ff783592b6b>`__,
`v5.17-rc2 <https://git.kernel.org/linus/49d766f3a0e49624c4cf83909d56c68164e7c545>`__,
`v5.17-rc3 <https://git.kernel.org/linus/86286e486cbdd68f01d330409307f6a6efcd4298>`__,
`v5.17-rc5 <https://git.kernel.org/linus/705d84a366cfccda1e7aec1113a5399cd2ffee7d>`__,
`v5.17-rc6 <https://git.kernel.org/linus/c0419188b5c1a7735b12cf1405cafc3f8d722819>`__,
`v5.17-rc7 <https://git.kernel.org/linus/3ee65c0f0778b8fa95381cd7676cde2c03e0f889>`__

Features:

- make send work with concurrent block group relocation
- new exclusive operation 'balance paused' to allow adding a device to
  filesystem with paused balance
- new sysfs file for fsid stored in the per-device directory to help
  distinguish devices when seeding is enabled

Performance:

- less metadata needed for directory logging, directory deletion is 20-40% faster
- in zoned mode, cache zone information during mount to speed up repeated
  queries (about 50% speedup)
- free space tree entries get indexed and searched by size (latency -30%,
  search run time -30%)
- less contention in tree node locking when inserting a key and no splits are
  needed (files/sec in fsmark improves by 1-20%)

Fixes:

- defrag rewrite from 5.16 fixed
- get rid of warning when mounted with flushoncommit

Core:

- global reserve stealing got simplified and cleaned up in evict
- more preparatory work for extent tree v2
- remove readahead framework
- error handling improvements
- for other changes see the [https://git.kernel.org/linus/d601e58c5f2901783428bc1181e83ff783592b6b pull request]

5.18 (May 2022)
^^^^^^^^^^^^^^^

Pull requests:
`v5.18-rc1 <https://git.kernel.org/linus/5191290407668028179f2544a11ae9b57f0bcf07>`__,
`v5.18-rc2 <https://git.kernel.org/linus/ce4c854ee8681bc66c1c369518b6594e93b11ee5>`__,
`v5.18-rc3 <https://git.kernel.org/linus/722985e2f6ec9127064771ba526578ea8275834d>`__,
`v5.18-rc5 <https://git.kernel.org/linus/fd574a2f841c8f07b20e5b55391e0af5d39d82ff>`__,
`v5.18-rc6 <https://git.kernel.org/linus/9050ba3a61a4b5bd84c2cde092a100404f814f31>`__,
`v5.18-rc6 <https://git.kernel.org/linus/4b97bac0756a81cda5afd45417a99b5bccdcff67>`__

- encoded read/write ioctls, allows user space to read or write raw data
  directly to extents (now compressed, encrypted in the future), will be
  used by send/receive v2 where it saves processing time
- zoned mode now works with metadata DUP (the mkfs.btrfs default)
- allow reflinks/deduplication from two different mounts of the same
  filesystem
- error message header updates:

  - print error state: transaction abort, other error, log tree errors
  - print transient filesystem state: remount, device replace, ignored
    checksum verifications

- tree-checker: verify the transaction id of the to-be-written dirty
  extent buffer
- fsync speedups

  - directory logging speedups (up to -90% run time)
  - avoid logging all directory changes during renames (up to -60% run
    time)
  - avoid inode logging during rename and link when possible (up to -60%
    run time)
  - prepare extents to be logged before locking a log tree path
    (throughput +7%)
  - stop copying old file extents when doing a full fsync ()
  - improved logging of old extents after truncate

- remove balance v1 ioctl, superseded by v2 in 2012

Core, fixes:

- continued extent tree v2 preparatory work

  - disable features that won't work yet
  - add wrappers and abstractions for new tree roots

- prevent deleting subvolume with active swapfile
- remove device count in superblock and its item in one transaction so
  they can't get out of sync
- for subpage, force the free space v2 mount to avoid a warning and
  make it easy to switch a filesystem on different page size systems
- export sysfs status of exclusive operation 'balance paused', so the
  user space tools can recognize it and allow adding a device with
  paused balance

5.19 (Jul 2022)
^^^^^^^^^^^^^^^

Pull requests:
`v5.19-rc1 <https://git.kernel.org/linus/bd1b7c1384ec15294ee45bf3add7b7036e146dad>`__,
`v5.19-rc4 <https://git.kernel.org/linus/ff872b76b3d89a09a997cc45c133e4a3ddc12f90>`__,
`v5.19-rc4 <https://git.kernel.org/linus/82708bb1eb9ebc2d1e296f2c919685761f2fa8dd>`__,
`v5.19-rc7 <https://git.kernel.org/linus/5a29232d870d9e63fe5ff30b081be6ea7cc2465d>`__,
`v5.19-rc7 <https://git.kernel.org/linus/972a278fe60c361eb8f37619f562f092e8786d7c>`__

Features:

- subpage:

  - support on PAGE_SIZE > 4K (previously only 64K)
  - make it work with raid56
  - prevent remount with v1 space cache

- repair super block num_devices automatically if it does not match
  the number of device items
- defrag can convert inline extents to regular extents, up to now inline
  files were skipped but the setting of mount option max_inline could
  affect the decision logic

- zoned:

  - minimal accepted zone size is explicitly set to 4MiB
  - make zone reclaim less aggressive and don't reclaim if there are
    enough free zones
  - add per-profile sysfs tunable of the reclaim threshold

- allow automatic block group reclaim for non-zoned filesystems, with
  sysfs tunables
- tree-checker: new check, compare extent buffer owner against owner
  rootid

Performance:

- avoid blocking on space reservation when doing nowait direct io
  writes, (+7% throughput for reads and writes)
- NOCOW write throughput improvement due to refined locking (+3%)
- send: reduce pressure to page cache by dropping extent pages right
  after they're processed

4.x
---

4.0 (Apr 2015)
^^^^^^^^^^^^^^

- file creation time is stored (no easy interface to read it yet)
- fsync and log replay fixes
- lots of cleanups and other fixes

4.1 (Jun 2015)
^^^^^^^^^^^^^^

Fixes:

- regression in chunk removal, conversion to raid1 possible again
- log tree corruption fix with ''-o discard'' mount
- bare xattr namespace attribute is not accepted
- orphan cleanup is started for implicitly mounted default subvolume
- send fixes
- cloning within same file
- EXTENT_SAME ioctl infinite loop fix
- avoid more ENOSPC in delayed-iput context
- a few ENOMEM fixes
- 'automatic empty block group removal' fixups

Speedups:

- large file deletion: run delayed refs more often
- large file deletion: don't build up too much work from crc
- transaction commit latency improved
- block group cache writeout

Qgroup:

- limits are shared upon snapshot
- allow to remove qgroup which has parent but no child
- fix status of qgroup consistency after rescan
- fix quota status bits after disabling
- mark qgroups inconsistent after assign/delete actions
- code cleanups

4.2 (Aug 2015)
^^^^^^^^^^^^^^

Enhancements:

- transaction abort now reports the caller, not the helper function
- INO_LOOKUP ioctl: unprivileged if used to just get the rootid (aka. subvolume id)
- unified ''subvol='' and ''subvolid='' mounting, show the mounted subvol in
  mount options; also, ''/proc/self/mountinfo'' now always correctly shows the
  mounted subvolume
- reworked internal qgroup logic
- send: use received_uuid of parent during send
- sysfs: preparatory works for exporting more stats about devices
- deduplication on the same inode works
- deduplication does not change mtime/ctime

Fixes:

- in send: cloning, renames, orphans
- few more ENOSPC fixes in case of block group creation/removal
- fix hang during inode eviction due to concurrent readahead
- EXTENT_SAME ioctl: handle unaligned length
- more fixes around automatic block group removal
- deadlock with EXTENT_SAME and readahead
- for feature NO_HOLES: fsync, truncate

4.3 (Nov 2015)
^^^^^^^^^^^^^^

- fix raid56 rebuild with missing device
- discard ioctl will return the number of bytes
- more bugfixes and cleanups

4.4 (Jan 2016)
^^^^^^^^^^^^^^

- send fixes: cloning, sending with parent
- improved handling of fragmented space using bitmaps
- new mount option for debugging: fragment=data|metadata|all
- updated balance filters: limit, stripes, usage
- more bugfixes and cleanups

4.5 (Mar 2016)
^^^^^^^^^^^^^^

- free space cache v2: an incompat feature to track the free space cache as a b-tree
- balance:
  - '-dconvert=dup' supported
  - continue but warn if metadata have lower redundancy than data
- fix: trim does not overwrite bootloader area (introduced in 4.3, fixed in 4.4.x stable kernels)
- assorted bugfixes, improvements or cleanups

4.6 (May 2016)
^^^^^^^^^^^^^^

- mount options:

  - usebackuproot - replace 'recovery' (works but is deprecated)
  - logreplay, nologreplay - disable log replay at mount time, does no writes to the device
  - norecovery - synthetic option to disable recovery at mount time and disable
    writes (now does: nologreplay)

- default inline limit is now 2048 (instead of page size, usually 4096)
- /dev/btrfs-control now understands the GET_SUPPORTED_FEATURES ioctl
- get rid of harmless message "''could not find root %llu''"
- preparatory work for subpage-blocksize patchset
- fix bug when using overlayfs
- fixes in readahead, log replay, fsync, and more

4.7 (Jul 2016)
^^^^^^^^^^^^^^

- allow balancing to dup with multi-device
- device deletion by id (additionally to by path)
- renameat2: add support for RENAME_EXCHANGE and RENAME_WHITEOUT
- enhanced selftests
- more preparatory work for "blocksize < page size"
- more validation checks of superblock (discovered by fuzzing)
- advertise which crc32c implementation is being used at module load
- fixed space report by ''df'' with mixed block groups
- log replay fixes
- device replace fixes

4.8 (Oct 2016)
^^^^^^^^^^^^^^

- space reservations and handling uses ticketed system, this should improve
  latency and fairness in case when there are several threads blocked on
  flushing
- fixes of bugs triggered by fuzzed images
- global ratelmit of all printed messages
- several send, qgroup fixes
- cleanups

4.9 (Dec 2016)
^^^^^^^^^^^^^^

- improved performance of extent sharing detection in FIEMAP

Fixes:

- device delete hang at the end of the operation
- free space tree bitmap endianness fixed on big-endian machines
- parallel incremental send and balance issue fixed
- cloning ioctl can be interrupted by a fatal signal
- other stability fixes or cleanups

4.10 (Feb 2017)
^^^^^^^^^^^^^^^

- balance: human readable block group description in the log
- balance: fix storing of stripes_min, stripes_max filters to the on-disk item
- qgroup: fix accounting bug during concurrent balance run
- better worker thread resource limit checks
- fix ENOSPC during hole punching
- fix ENOSPC when reflinking a heavily fragmented file
- fix crash when certain tracepoints are enabled
- fix compat ioctl calls on non-compat systems
- improved delayed ref iteration performance
- many cleanups

4.11 (May 2017)
^^^^^^^^^^^^^^^

- mostly a cleanup release
- improved csum mismatch messages
- move some qgroup work out of transaction commit
- let unlink temporarily exceed quotas
- fix truncate and lockless DIO writes
- incremental send fixes
- fix remount using ssd and nossd combinations

4.12 (Jul 2017)
^^^^^^^^^^^^^^^

- new tracepoints: file item
- fix qgroup accounting when inode_cache is in use
- fix incorrect number report in stat::t_blocks under certain conditions
- raid56 fixes:

  - enable auto-repair during read (ie. similar to what raid1 and raid10 do)
  - fix potential crash with concurrent scrub and dev-replace
  - fix potential crash when cancelling dev-replace
  - fix false reports during scrub when it's possible to do repair
  - fix wrong mirror report during repair

- many cleanups

4.13 (Sep 2017)
^^^^^^^^^^^^^^^

- deprecated: mount option ''alloc_start''
- qgroups: new sysctl to allow temporary quota override with CAP_SYS_RESOURCE
- statx syscall support
- nowait AIO support
- lots of cleanups around bio processing and error handling
- memory allocation constraint cleanups and improvements
- more sanity checks (for dir_item)
- compression will be skipped if there's no improvement (at least one block)
- fix invalid extent maps due to hole punching
- fix: sgid not cleared when changing acls
- some enospc corner case fixes
- send fixes
- other cleanups

4.14 (Nov 2017)
^^^^^^^^^^^^^^^

- added zstd compression
- fine-grained check for degraded mount (verify raid constraints on chunk level, not device level)
- userspace transaction ioctl has been deprecated, scheduled for removal in 4.17
- foundation code for compression heuristics
- mount option 'ssd' does not force block allocation alignments

Fixes:

- potential raid repair and compression crash
- prevent to set invalid default subvolid
- resume qgroup rescan on rw remount
- better reporting of detected checksum mismatches for DIO
- compression for defrag vs per-file behaves as expected, respecting the requested value
- possible deadlock with readdir and pagefault
- emission of invalid clone operations in send
- cleanups and refactoring

4.15 (Jan 2018)
^^^^^^^^^^^^^^^

New features:

- extend mount options to specify zlib compression level, <i>-o compress=zlib:9</i>
- v2 of ioctl "extent to inode mapping"
- populate compression heuristics logic
- enable indexing for btrfs as lower filesystem in overlayfs
- speedup page cache readahead during send on large files

Internal changes:

- more sanity checks of b-tree items when reading them from disk
- more EINVAL/EUCLEAN fixups, missing BLK_STS_* conversion, other errno or error handling fixes
- remove some homegrown IO-related logic, that's been obsoleted by core block
  layer changes (batching, plug/unplug, own counters)
- add ref-verify, optional debugging feature to verify extent reference accounting
- simplify code handling outstanding extents, make it more clear where and how the accounting is done
- make delalloc reservations per-inode, simplify the code and make the logic more straightforward
- extensive cleanup of delayed refs code
- fix send ioctl on 32bit with 64bit kernel

4.16 (Apr 2018)
^^^^^^^^^^^^^^^

- fallocate: implement zero range mode
- avoid losing data raid profile when deleting a device
- tree item checker: more checks for directory items and xattrs
- raid56 recovery: don't use cached stripes, that could be potentially changed
  and a later RMW or recovery would lead to corruptions or failures
- let raid56 try harder to rebuild damaged data, reading from all stripes if necessary
- fix scrub to repair raid56 in a similar way as in the case above
- cleanups: device freeing, removed some call indirections, redundant
  bio_put/_get, unused parameters, refactorings and renames
- RCU list traversal fixups
- simplify mount callchain, remove recursing back when mounting a subvolume
- plug for fsync, may improve bio merging on multiple devices
- compression heuristic: replace heap sort with radix sort, gains some performance
- add extent map selftests, buffered write vs dio
- see [https://git.kernel.org/linus/31466f3ed710e5761077190809e694f55aed5deb pull request]

4.17 (Jun 2018)
^^^^^^^^^^^^^^^

- mount options: new nossd_spread; subvolid will detect junk after the number and fail the mount
- add message after cancelled device replace
- direct module dependency on libcrc32, removed own crc wrappers
- removed user space transaction ioctls
- use lighter locking when reading /proc/self/mounts (RCU)
- skip writeback of last page when truncating file to same size
- send: do not issue unnecessary truncate operations
- selftests: more tree block validation
- fix fsync after hole punching when using no-holes feature
- raid56:

  - make sure target is identical to source when raid56 rebuild fails after dev-replace
  - faster rebuild during scrub, batch by stripes and not block-by-block
  - make more use of cached data when rebuilding from a missing device

- [https://git.kernel.org/linus/94514bbe9e5c402c4232af158a295a8fdfd72a2c pull request]

4.18 (Aug 2018)
^^^^^^^^^^^^^^^

- added support for the ioctl FS_IOC_FSGETXATTR, per-inode flags, successor of
  GET/SETFLAGS; now supports only existing flags: append, immutable, noatime,
  nodump, sync
- 3 new unprivileged ioctls to allow users to enumerate subvolumes
- dedupe syscall implementation does not restrict the range to 16MiB, though it still splits the whole range to 16MiB chunks
- on user demand, rmdir() is able to delete an empty subvolume, export the capability in sysfs
- fix inode number types in tracepoints, other cleanups
- send: improved speed when dealing with a large removed directory,
  measurements show decrease from 2000 minutes to 2 minutes on a  directory
  with 2 million entries
- pre-commit check of superblock to detect a mysterious in-memory corruption
- log message updates
- [https://git.kernel.org/linus/704996566f97e0e24c97052f81678060c213c260 pull request]

4.19 (Oct 2018)
^^^^^^^^^^^^^^^

Highlights, no big changes in this release:

- allow defrag on opened read-only files that have rw permissions
- tree checker improvements, reported by fuzzing
- send, fix incorrect file layout after hole punching beyond eof
- reset on-disk device stats value after replace
- assorted fixes, cleanups and dead code removal
- [https://git.kernel.org/linus/318b067a5dd649d198c2ba00cf7408d778fc00b4 pull request]

4.20 (Dec 2018)
^^^^^^^^^^^^^^^

Performance improvements:

- fewer wakeups and blocking during b-tree traversals, improved latencies and scalability
- qgroups: 30+% run time improvement during balance, no accounting on unchanged subtrees (continued)
- use a cached variant of rb-tree, speeds up traversal in some cases

Fixes:

- trim:

  - could miss some block groups, if logical offset was too high and did not fit the range
  - better error reporting, continue as far as possible
  - less interaction with transaction commit

- fsync: fix log replay and O_TMPFILE warnings
- qgroups: fix rescan that might misc some dirty groups
- don't clean dirty pages during buffered writes, this could lead to lost updates in some corner cases
- some block groups could have been delayed in creation, if the allocation triggered another one
- error handling improvements
- other cleanups and refactoring
- [https://git.kernel.org/linus/a1a4f841ec4585185c0e75bfae43a18b282dd316 pull request]

3.x
---

3.0 (Jul 2011)
^^^^^^^^^^^^^^

* Filesystem scrub
* Auto-defragmentation (autodefrag mount option)
* Improved block allocator
* Sped up file creation/deletion by delayed operation

3.1 (Oct 2011)
^^^^^^^^^^^^^^

* Stability fixes (lots of them, really), notably fixing early ENOSPC, improved
  handling of a few error paths and corner cases, fix for the crash during log
  replay.

3.2 (Jan 2012)
^^^^^^^^^^^^^^

* Log of past roots to aid recovery (option ''recovery'')
* Subvolumes mountable by full path
* Added ''nospace_cache'' option
* Lots of space accounting fixes
* Improved scrub performance thanks to new read-ahead infrastructure
* Scrub prints paths of corrupted files
* ioctl for resolving logical->inode and inode->path
* Integrated raid-repair (if possible)
* Data corruption fix for parallel snapshot creation
* Write barriers for multiple devices were fixed to be more resistant in case of power failure

3.3 (Mar 2012)
^^^^^^^^^^^^^^

* restriper - infrastructure to change btrfs raid profiles on the fly via balance
* optional integrity checker infrastructure (http://lwn.net/Articles/466493)
* fixed a few corner cases where TRIM did not process some blocks
* cluster allocator improvements (less fragmentation, some speedups)

3.4 (May 2012)
^^^^^^^^^^^^^^

* Allow metadata blocks larger than the page size (4K). This allows metadata
  blocks up to 64KB in size. In practice 16K and 32K seem to work best. For
  workloads with lots of metadata, this cuts down the size of the extent
  allocation tree dramatically and fragments much less. (Chris Mason)
* Improved error handling (IO errors). This gives Btrfs the ability to abort
  transactions and go read-only on errors other than internal logic errors and
  ENOMEM more gracefully instead of crashing. (Jeff Mahoney)
* Reworked the way in which metadata interacts with the page cache.
  page->private now points to the btrfs extent_buffer object, which makes
  everything faster. The code was changed so it now writes a whole extent
  buffer at a time instead of allowing individual pages to go down. It is now
  more aggressive about dropping pages for metadata blocks that were freed due
  to COW. Overall, metadata caching is much faster now. (Josef Bacik)

3.5 (Jun 2012)
^^^^^^^^^^^^^^

* collect device statistics (read/write failures, checksum errors, corrupted blocks)
* integrity checker (3.3+) supports bigblocks (3.4+)
* more friendly NFS support (native ''i_version'')
* ''thread_pool'' mount option tunable via remount
* ''fsync'' speed improvements
* several fixes related to read-only mounts
* scrub thread priority lowered to idle
* preparatory works for 3.6 features (''tree_mod_log'')

3.6 (Sep 2012)
^^^^^^^^^^^^^^

* subvolume-aware quotas (''qgroups'')
* support for send/receive between snapshot changes (http://lwn.net/Articles/506244)
* ''atime'' is not updated on read-only snapshots (http://lwn.net/Articles/499293)
* allowed cross-subvolume file clone (aka. reflink)
* remount with ''no'' compression possible
* new ioctl to read device readiness status
* speed improvement for concurrent multithreaded reads

3.7 (Dec 2012)
^^^^^^^^^^^^^^

* ''fsync'' speedups
* removed limitation of number of hardlinks in a single directory
* file hole punching (http://lwn.net/Articles/415889)
* per-file ''NOCOW''
* fixes to send/receive

3.8 (Feb 2013)
^^^^^^^^^^^^^^

* ability to replace devices at runtime in an effective way (http://lwn.net/Articles/524589)
* speed improvements (cumulative effect of many small improvements)
* a few more bugfixes

3.9 (Apr 2013)
^^^^^^^^^^^^^^

* preliminary Raid 5/6 support (details in http://www.spinics.net/lists/linux-btrfs/msg22169.html)
* snapshot-aware defrag
* a mode of ''send'' to avoid transferring file data
* direct IO speedup (https://patchwork.kernel.org/patch/2114921)
* new ''ioctl''s to set/get filesystem label
* defrag is cancellable

3.10 (Jun 2013)
^^^^^^^^^^^^^^^

* reduced size of metadata by so-called :ref:`skinny extents<mkfs-feature-skinny-metadata>` (http://git.kernel.org/linus/3173a18f70554fe7880bb2d85c7da566e364eb3c)
* enhanced syslog message format (http://permalink.gmane.org/gmane.comp.file-systems.btrfs/24330)
* the mount option ''subvolrootid'' is deprecated
* lots of stability improvements, removed many< BUG_ONs
* qgroups are automatically created when quotas are enabled (http://git.kernel.org/linus/7708f029dca5f1b9e9d6ea01ab10cd83e4c74ff2)
* qgroups are able to ''rescan'' current filesystem and sync the quota state with the existing subvolumes
* enhanced ''send/recv '' format for multiplexing more data into one stream (http://git.kernel.org/linus/c2c71324ecb471c932bc1ff59e46ffcf82f274fc)
* various unsorted code cleanups, minor performance updates

3.11 (Sep 2013)
^^^^^^^^^^^^^^^

* extent cloning within one file
* ioctl to wait for quota rescan completion
* device deletion returns error code to userspace (not in syslog anymore)
* usual load of small fixes and improvements

3.12 (Nov 2013)
^^^^^^^^^^^^^^^

* Major performance improvement for send/receive with large numbers of subvolumes
* Support for batch :doc:`deduplication<Deduplication>` (userspace tools required)
* new mount option ''commit'' to set the commit interval
* Lots of stability and bugfix patches

3.13 (Jan 2014)
^^^^^^^^^^^^^^^

* ''fiemap'' exports information about shared extents
* bugfix and stability focused release

3.14 (Mar 2014)
^^^^^^^^^^^^^^^

* optional incompat disk format improvement aiming at speedup, removing file hole representation, named ''no-holes''
* ioctl to query/change feature bits (e.g. switching on extended refs on-line now possible)
* export filesystem info through sysfs: features, allocation profiles
* added pairing mount options (for remount)
* heap of small performance optimizations
* snapshot-aware defrag was disabled due to problems

3.15 (Jun 2014)
^^^^^^^^^^^^^^^

* pile of ''send'' fixes (stability, speed)
* worker threads now use kernel workqueues

3.16 (Aug 2014)
^^^^^^^^^^^^^^^

* ''O_TMPFILE'' support (http://kernelnewbies.org/Linux_3.11#head-8be09d59438b31c2a724547838f234cb33c40357)
* reworked qgroup accounting, to fix negative numbers after subvol deletion
* SEARCH_TREE ioctl v2, extended for retrieving more data (http://www.spinics.net/lists/linux-btrfs/msg31213.html)
* new balance filter ''limit'' for more finegrained balancing (http://www.spinics.net/lists/linux-btrfs/msg33872.html)
* ioctl FS_INFO and it's sysfs counterpart export information about ''nodesize'', ''sectorsize'' and ''clone_alignment''
* snapshots are protected during send

3.17 (Oct 2014)
^^^^^^^^^^^^^^^

* fix for the infamous deadlock (https://git.kernel.org/linus/9e0af23764344f7f1b68e4eefbe7dc865018b63d)
* fixed longstanding bug in qgroups accounting after snapshot deletion (https://git.kernel.org/linus/1152651a081720ef6a8c76bb7da676e8c900ac30)
* updated (less inaccurate) ''df'' numbers (https://git.kernel.org/linus/ba7b6e62f420f5a8832bc161ab0c7ba767f65b3d)
* speedup for ''rename'' and ''truncate'', less strict flushes (https://git.kernel.org/linus/8d875f95da43c6a8f18f77869f2ef26e9594fecc)
* updated and fixes to the ''seeding'' feature

3.17 (Oct 2014)
^^^^^^^^^^^^^^^

* fix for the infamous deadlock (https://git.kernel.org/linus/9e0af23764344f7f1b68e4eefbe7dc865018b63d]
* fixed longstanding bug in qgroups accounting after snapshot deletion (https://git.kernel.org/linus/1152651a081720ef6a8c76bb7da676e8c900ac30)
* updated (less inaccurate) ''df'' numbers (https://git.kernel.org/linus/ba7b6e62f420f5a8832bc161ab0c7ba767f65b3d)
* speedup for ''rename'' and ''truncate'', less strict flushes (https://git.kernel.org/linus/8d875f95da43c6a8f18f77869f2ef26e9594fecc)
* updated and fixes to the ''seeding'' feature

3.18 (Dec 2014)
^^^^^^^^^^^^^^^

3.19 (Feb 2015)
^^^^^^^^^^^^^^^

* raid56 supports scrub and device replace

2.6.x
-----

2.6.39 (May 2011)
^^^^^^^^^^^^^^^^^

Per-file compression and NOCOW control. Support for bulk TRIM on SSDs.

2.6.38 (March 2011)
^^^^^^^^^^^^^^^^^^^

Added LZO compression method, FIEMAP bugfixes with delalloc, subvol flags
get/set ioctl, allow compression during defrag.

2.6.37 (January 2011)
^^^^^^^^^^^^^^^^^^^^^

On-disk free space cache, asynchronous snapshots, unprivileged subvolume
deletion, extent buffer switches from a rbtree with spinlocks to a radix tree
with RCU.

2.6.35 (August 2010)
^^^^^^^^^^^^^^^^^^^^

Direct I/O support and -ENOSPC handling of volume management operations,
completing the -ENOSPC support.

2.6.34 (May 2010)
^^^^^^^^^^^^^^^^^

Support for changing the default subvolume, a new userspace tool (btrfs), an
ioctl that lists all subvolumes, an ioctl to allow improved df math, and other
improvements.

2.6.33 (February 2010)
^^^^^^^^^^^^^^^^^^^^^^

Some minor -ENOSPC improvements.

2.6.32 (December 2009)
^^^^^^^^^^^^^^^^^^^^^^

ENOSPC

Btrfs has not had serious -ENOSPC ("no space") handling, the COW oriented
design makes handling such situations more difficult than filesystems that just
rewrite the blocks. In this release Josef Bacik (Red Hat) has added the
necessary infrastructure to fix that problem. Note: The filesystem may run out
of space and still show some free space. That space comes from a data/metadata
chunk that can't get filled because there's not space left to create its
metadata/data counterpart chunk. This is unrelated to the -ENOSPC handling and
will be fixed in the future. Code:
(http://git.kernel.org/linus/9ed74f2dba6ebf9f30b80554290bfc73cc3ef083)

Proper snapshot and subvolume deletion

In the last btrfs-progs version you have options that allow to delete snapshots
and subvolumes without having to use rm. This is much faster because it does
the deletion via btree walking. It's also now possible to rename snapshots and
subvols. Work done by Yan Zheng (Oracle). Code:
(http://git.kernel.org/linus/4df27c4d5cc1dda54ed7d0a8389347f2df359cf9,
http://git.kernel.org/linus/76dda93c6ae2c1dc3e6cde34569d6aca26b0c918)

Performance improvements

Streaming writes on very fast hardware were previously CPU bound at around
400MB/s. Chris Mason (Oracle) has improved the code so that now it can push
over 1GB/s while using the same CPU as XFS (factoring out checksums). There are
also improvements for writing large portions of extents, and other workloads.
Multidevice setups are also much faster due to the per-BDI writeback changes.
The performance of fsync() was greatly improved, which fixed a severe slowdown
while using yum in Fedora 11.

Support for "discard" operation on SSD devices

"Discard" support is a way to telling SSD devices which blocks are free so that
the underlying firmware knows that it's safe to do some optimizations
(http://git.kernel.org/linus/e244a0aeb6a599c19a7c802cda6e2d67c847b154,
http://git.kernel.org/linus/0634857488ec6e28fa22920cd0bee3c2ac07ccfd )

0.x
---

0.13 and older
^^^^^^^^^^^^^^

* Copy on write FS
* Checksumming
* Transactions
* Snapshotting
* Subvolumes

0.14 (April 30, 2008)
^^^^^^^^^^^^^^^^^^^^^

* Support for multiple devices
* raid0, raid1 and raid10, single spindle metadata duplication

0.15 (May 29, 2008)
^^^^^^^^^^^^^^^^^^^

* Metadata back references
* Online growing and shrinking
* Conversion program from Ext3
* data=ordered support
* COW-free data writes.
* focus on stability fixes for the multiple device code

0.16 (August 2008)
^^^^^^^^^^^^^^^^^^

v0.16 does change the disk format from v0.15, and it includes a long list of
performance and stability updates.

Fine grained Btree locking

Locking is now done in a top down fashion while searching the btree, and higher
level locks are freed when they are no longer required. Extent allocations
still have a coarse grained lock, but that will be improved in the next
release.

Improved data=ordered

Ordered data mode loosely means any system that prevents garbage or stale data
blocks after a crash. It was previously implemented the same way ext3 does it,
which is to force pending data writes down before a transaction commits.

The data=ordered code was changed to only modify metadata in the btree after
data extents are fully written on disk. This allows a transaction commit to
proceed without waiting for all the data writes on the FS to finish.

A single fsync or synchronous write no longer forces all the dirty data on the
FS to disk, as it does in ext3 and reiserfsv3.

Although it is not implemented yet, the new data=ordered code would allow
atomic writes of almost any size to a single file to be exported to userland.

ACL support (Josef Bacik)

ACLs are implemented and enabled by default.

Lost file prevention (Josef Bacik)

The VFS and posix APIs force filesystems allow files to be unlinked from a
directory before they are deleted from the FS. If the system crashes between
the unlink and the deletion, the file is still consuming space on disk, but not
listed in any directory.

Btrfs now tracks these files and makes sure they are reclaimed if the system
crashes before they are fully deleted.

New directory index format (Josef Bacik)

Btrfs indexes directories in two ways. The first index allows fast name
lookups, and the second is optimized to return inodes in something close to
disk order for readdir. The second index is an important part of good
performance for full filesystem backups.

A per-directory sequence number is now used for the second index, removing some
worst case conditions around files that are hard linked into the same directory
many times.

Faster unmount times (Yan Zheng)

Btrfs waits for old transactions to be completely removed from the FS before
unmount finishes. A new reference count cache was added to make this much less
IO intensive, improving FS performance in all workloads.

Improved streaming reads and writes

The new data=ordered code makes streaming writes much faster. Streaming reads
are improved by tuning the thread pools used to process data checksums after
the read is done. On machines with sufficient CPU power to keep up with the
disks, data checksumming is able to run as fast as nodatasum mounts.

0.17 (January 2009)
^^^^^^^^^^^^^^^^^^^

Btrfs is now in 2.6.29-rc1!

v0.17 has a new disk format since v0.16. Future releases will try to maintain
backwards compatibility with this new format.

Compression

Transparent zlib compression of file data is enabled by mount -o compress.

Improved block allocation routines (Josef Bacik)

Many performance problems in the allocator are addressed in this release

Improved block sharing while moving extents (Yan Zheng)

The btrfs-vol commands to add, remove and balance space across devices triggers
a COW of metadata and data blocks. This release is much better at maintaining
shared blocks between snapshots when that COW happens.

Seed Device support

It is now possible to create a filesystem to seed other Btrfs filesystems. The
original filesystem and devices are included as a readonly starting point to
the new FS. All modifications go onto different devices and the COW machinery
makes sure the original is unchanged.

Many bug fixes and performance improvements

0.18 (January 2009)
^^^^^^^^^^^^^^^^^^^

v0.18 has the same disk format as 0.17, but a bug was found in the ioctl
interface shared between 32 bit and 64 bit programs. This was fixed by changing
the ioctl interface. Anyone using 2.6.29-rc2 will need to update to v0.18 of
the btrfs progs.

There is no need to reformat though, the disk format is still compatible.

0.19 (June 2009)
^^^^^^^^^^^^^^^^

v0.19 is a forward rolling format change, which means that it can read the
v0.18 disk format but older kernels and older btrfs-progs code will not be able
to read filesystems created with v0.19. The new code changes the way that
extent back references are recorded, making them significantly more efficient.
In general, v0.19 is a dramatic speed improvement over v0.18 in almost every
workload.

The v0.19 utilities are meant for use with kernels 2.6.31-rc1 and higher. Git
trees are available with the new format code for 2.6.30 kernels, please see the
download section for details.

If you do not wish to roll forward to the new disk format, use the v0.18 utilities.
