Status
======

Overview
--------

For a list of features by their introduction, please see `Changes (feature/version) <Feature-by-version>`__.

The table below aims to serve as an overview for the stability status of
the features BTRFS supports. While a feature may be functionally safe
and reliable, it does not necessarily mean that its useful, for example
in meeting your performance expectations for your specific workload.
Combination of features can vary in performance, the table does not
cover all possibilities.

**The table is based on the latest released linux kernel: 6.2**

The columns for each feature reflect the status of the implementation
in following ways:

| *Stability* - completeness of the implementation, use case coverage
| *Performance* - how much it could be improved until the inherent limits are hit
| *Notes* - short description of the known issues, or other information related to status

*Legend:*

-  **OK**: should be safe to use, no known major deficiencies
-  **mostly OK**: safe for general use, there are some known problems
   that do not affect majority of users
-  **Unstable**: do not use for other then testing purposes, known
   severe problems, missing implementation of some core parts

.. list-table::
   :header-rows: 1

   * - Feature
     - Stability
     - Performance
     - Notes
   * - discard (synchronous)
     - OK
     -
     - mounted with `-o discard` (has performance implications), also see `fstrim`
   * - discard (asynchronous)
     - OK
     -
     - mounted with `-o discard=async` (improved performance)"
   * - Autodefrag
     - OK
     -
     -
   * - Defrag
     - mostly OK
     -
     - extents get unshared (see below)
   * - Compression
     - OK (4.14)
     -
     -
   * - Out-of-band dedupe
     - OK
     - mostly OK
     - (reflink), heavily referenced extents have a noticeable performance hit (see below)
   * - File range cloning
     - OK
     - mostly OK
     - (reflink), heavily referenced extents have a noticeable performance hit (see below)
   * - More checksumming algorithms
     - OK
     - OK
     -
   * - Auto-repair
     - OK
     - OK
     - automatically repair from a correct spare copy if possible (DUP, RAID1, RAID10, RAID56)
   * - Scrub
     - OK
     - OK
     -
   * - Scrub + RAID56
     - mostly OK
     - mostly OK
     -
   * - nodatacow
     - OK
     - OK
     -
   * - Device replace
     - mostly OK
     - mostly OK
     - (see below)
   * - Degraded mount
     - OK (4.14)
     - n/a
     -
   * - Single (block group profile)
     - OK
     - OK
     -
   * - DUP (block group profile)
     - OK
     - OK
     -
   * - RAID0
     - OK
     - OK
     -
   * - RAID1
     - OK
     - mostly OK
     - reading from mirrors in parallel can be optimized further (see below)
   * - RAID1C3
     - OK
     - mostly OK
     - reading from mirrors in parallel can be optimized further (see below)
   * - RAID1C4
     - OK
     - mostly OK
     - reading from mirrors in parallel can be optimized further (see below)
   * - RAID10
     - OK
     - mostly OK
     - reading from mirrors in parallel can be optimized further (see below)
   * - RAID56
     - unstable
     - n/a
     - (see below)
   * - Mixed block groups
     - OK
     - OK
     -
   * - Filesystem resize
     - OK
     - OK
     - shrink, grow
   * - Balance
     - OK
     - OK
     - balance + qgroups can be slow when there are many snapshots
   * - Offline UUID change
     - OK
     - OK
     -
   * - Metadata UUID change
     - OK
     - OK
     -
   * - Subvolumes, snapshots
     - OK
     - OK
     -
   * - Send
     - OK
     - OK
     -
   * - Receive
     - OK
     - OK
     -
   * - Seeding
     - OK
     - OK
     -
   * - Quotas, qgroups
     - mostly OK
     - mostly OK
     - qgroups with many snapshots slows down balance
   * - Swapfile
     - OK
     - n/a
     - with some limitations
   * - NFS
     - OK
     - OK
     -
   * - cgroups
     - OK
     - OK
     - IO controller
   * - Samba
     - OK
     - OK
     - compression, server-side copies, snapshots
   * - io_uring
     - OK
     - OK
     -
   * - fsverity
     - OK
     - OK
     -
   * - idmapped mount
     - OK
     - OK
     -
   * - Free space tree
     - OK (4.9)
     -
     -
   * - no-holes
     - OK
     - OK
     -
   * - skinny-metadata
     - OK
     - OK
     -
   * - extended-refs
     - OK
     - OK
     -
   * - zoned mode
     - mostly OK
     - mostly OK
     - there are known bugs, use only for testing

Please open an issue if:

-  there's a known missing entry
-  a particular feature combination that has a different status and is
   worth mentioning separately
-  you know of a bug that lowers the feature status
-  a reference could be enhanced by an actual link to documentation
   (wiki, manual pages)


Details that do not fit the table
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Defrag
^^^^^^

The data affected by the defragmentation process will be newly written
and will consume new space, the links to the original extents will not
be kept. See also :doc:`btrfs-filesystem<btrfs-filesystem>` . Though
autodefrag affects newly written data, it can read a few adjacent blocks
(up to 64KiB) and write the contiguous extent to a new location. The
adjacent blocks will be unshared. This happens on a smaller scale than
the on-demand defrag and doesn't have the same impact.


RAID1, RAID10
^^^^^^^^^^^^^

The simple redundancy RAID levels utilize different mirrors in a way
that does not achieve the maximum performance. The logic can be improved
so the reads will spread over the mirrors evenly or based on device
congestion.

RAID56
^^^^^^

Please see
https://btrfs.readthedocs.io/en/latest/btrfs-man5.html#raid56-status-and-recommended-practices
.


Device replace
^^^^^^^^^^^^^^

Device *replace* and device *delete* insist on being able to read or
reconstruct all data. If any read fails due to an IO error, the
delete/replace operation is aborted and the administrator must remove or
replace the damaged data before trying again.


On-disk format
--------------

The filesystem disk format is stable. This means it is not expected to
change unless there are very strong reasons to do so. If there is a
format change, filesystems which implement the previous disk format will
continue to be mountable and usable by newer kernels.

The core of the on-disk format that comprises building blocks of the
filesystem:

-  layout of the main data structures, eg. superblock, b-tree nodes,
   b-tree keys, block headers
-  the COW mechanism, based on the original design of Ohad Rodeh's paper
   "Shadowing and clones"

Newly introduced features build on top of the above and could add
specific structures. If a backward compatibility is not possible to
maintain, a bit in the filesystem superblock denotes that and the level
of incompatibility (full, read-only mount possible).
