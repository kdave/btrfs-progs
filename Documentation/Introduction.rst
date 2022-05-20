Introduction
============

BTRFS is a modern copy on write (COW) filesystem for Linux aimed at
implementing advanced features while also focusing on fault tolerance, repair
and easy administration. Its main features and benefits are:

* Snapshots which do not make the full copy of files
* Built-in volume management, support for software-based RAID 0, RAID 1, RAID 10 and others
* Self-healing - checksums for data and metadata, automatic detection of silent data corruptions

Feature overview:

* Extent based file storage
* 2\ :sup:`64` byte == 16 EiB maximum file size (practical limit is 8 EiB due to Linux VFS)
* Space-efficient packing of small files
* Space-efficient indexed directories
* Dynamic inode allocation
* Writable snapshots, read-only snapshots
* Subvolumes (separate internal filesystem roots)
* Checksums on data and metadata (crc32c, xxhash, sha256, blake2b)
* Compression (ZLIB, LZO, ZSTD), heuristics
* Integrated multiple device support
   * File Striping
   * File Mirroring
   * File Striping+Mirroring
   * Single and Dual Parity implementations (experimental, not production-ready)
* SSD (flash storage) awareness (TRIM/Discard for reporting free blocks for
  reuse) and optimizations (e.g. avoiding unnecessary seek optimizations,
  sending writes in clusters, even if they are from unrelated files. This
  results in larger write operations and faster write throughput)
* Efficient incremental backup
* Background scrub process for finding and repairing errors of files with redundant copies
* Online filesystem defragmentation
* Offline filesystem check
* In-place conversion of existing ext2/3/4 and reiserfs file systems
* Seed devices. Create a (readonly) filesystem that acts as a template to seed
  other Btrfs filesystems. The original filesystem and devices are included as
  a readonly starting point for the new filesystem. Using copy on write, all
  modifications are stored on different devices; the original is unchanged.
* Subvolume-aware quota support
* Send/receive of subvolume changes
   * Efficient incremental filesystem mirroring
* Batch, or out-of-band deduplication (happens after writes, not during)
* Swapfile support
* Tree-checker, post-read and pre-write metadata verification
* Zoned mode support (SMR/ZBC/ZNS friendly allocation)
