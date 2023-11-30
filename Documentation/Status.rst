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

**The table is based on the latest released linux kernel: 6.6**

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

.. role:: statusok
.. role:: statusmok
.. role:: statusunstable
.. role:: statusunsupp
.. role:: statusincompat

.. list-table::
   :header-rows: 1

   * - Feature
     - Stability
     - Performance
     - Notes
   * - :doc:`Subvolumes, snapshots<Subvolumes>`
     - :statusok:`OK`
     - OK
     -
   * - :doc:`Compression<Compression>`
     - :statusok:`OK`
     -
     -
   * - :doc:`Checksumming algorithms<Checksumming>`
     - :statusok:`OK`
     - OK
     -
   * - :doc:`Defragmentation<Defragmentation>`
     - :statusmok:`mostly OK`
     -
     - extents get unshared (see below)
   * - :ref:`Autodefrag<mount-option-autodefrag>`
     - :statusok:`OK`
     -
     -
   * - :doc:`Discard (synchronous)<Trim>`
     - :statusok:`OK`
     -
     - mounted with `-o discard` (has performance implications), also see `fstrim`
   * - :doc:`Discard (asynchronous)<Trim>`
     - :statusok:`OK`
     -
     - mounted with `-o discard=async` (improved performance)
   * - :doc:`Out-of-band dedupe<Deduplication>`
     - :statusok:`OK`
     - :statusmok:`mostly OK`
     - (reflink), heavily referenced extents have a noticeable performance hit (see below)
   * - :doc:`File range cloning<Reflink>`
     - :statusok:`OK`
     - :statusmok:`mostly OK`
     - (reflink), heavily referenced extents have a noticeable performance hit (see below)
   * - :doc:`Filesystem resize<Resize>`
     - :statusok:`OK`
     - OK
     - shrink, grow
   * - :doc:`Device replace<Volume-management>`
     - :statusmok:`mostly OK`
     - mostly OK
     - (see below)
   * - :doc:`Auto-repair<Auto-repair>`
     - :statusok:`OK`
     - OK
     - automatically repair from a correct spare copy if possible (DUP, RAID1, RAID10, RAID56)
   * - :doc:`Scrub<Scrub>`
     - :statusok:`OK`
     - OK
     -
   * - Scrub + RAID56
     - :statusmok:`mostly OK`
     - mostly OK
     -
   * - :ref:`Degraded mount<mount-option-degraded>`
     - :statusok:`OK`
     - n/a
     -
   * - :doc:`Balance<Balance>`
     - :statusok:`OK`
     - OK
     - balance + qgroups can be slow when there are many snapshots
   * - :doc:`Send<Send-receive>`
     - :statusok:`OK`
     - OK
     -
   * - :doc:`Receive<Send-receive>`
     - :statusok:`OK`
     - OK
     -
   * - Offline UUID change
     - :statusok:`OK`
     - OK
     -
   * - Metadata UUID change
     - :statusok:`OK`
     - OK
     -
   * - Temporary UUID
     - 6.7
     - 6.7
     -
   * - :doc:`Seeding<Seeding-device>`
     - :statusok:`OK`
     - OK
     -
   * - :doc:`Quotas, qgroups<Qgroups>`
     - :statusmok:`mostly OK`
     - mostly OK
     - qgroups with many snapshots slows down balance
   * - :doc:`Quotas, simple qgroups<Qgroups>`
     - 6.7
     - 6.7
     - simplified qgroup accounting, better performance
   * - :doc:`Swapfile<Swapfile>`
     - :statusok:`OK`
     - n/a
     - with some limitations
   * - nodatacow
     - :statusok:`OK`
     - OK
     -
   * - :doc:`Subpage block size<Subpage>`
     - :statusmok:`mostly OK`
     - mostly OK
     - Also see table below for more detailed compatibility.
   * - :doc:`Zoned mode<Zoned-mode>`
     - :statusmok:`mostly OK`
     - mostly OK
     - Not yet feature complete but moderately stable, also see table below
       for more detailed compatibility.

Block group profiles
^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1

   * - Feature
     - Stability
     - Performance
     - Notes
   * - :ref:`Single (block group profile)<mkfs-section-profiles>`
     - :statusok:`OK`
     - OK
     -
   * - :ref:`DUP (block group profile)<mkfs-section-profiles>`
     - :statusok:`OK`
     - OK
     -
   * - :ref:`RAID0<mkfs-section-profiles>`
     - :statusok:`OK`
     - OK
     -
   * - :ref:`RAID1<mkfs-section-profiles>`
     - :statusok:`OK`
     - mostly OK
     - reading from mirrors in parallel can be optimized further (see below)
   * - :ref:`RAID1C3<mkfs-section-profiles>`
     - :statusok:`OK`
     - mostly OK
     - reading from mirrors in parallel can be optimized further (see below)
   * - :ref:`RAID1C4<mkfs-section-profiles>`
     - :statusok:`OK`
     - mostly OK
     - reading from mirrors in parallel can be optimized further (see below)
   * - :ref:`RAID10<mkfs-section-profiles>`
     - :statusok:`OK`
     - mostly OK
     - reading from mirrors in parallel can be optimized further (see below)
   * - :ref:`RAID56<mkfs-section-profiles>`
     - :statusunstable:`unstable`
     - n/a
     - (see below)
   * - :ref:`Mixed block groups<mkfs-feature-mixed-bg>`
     - :statusok:`OK`
     - OK
     -


On-disk format
^^^^^^^^^^^^^^

Features that are typically set at *mkfs* time (sometimes can be changed or
converted later).

.. list-table::
   :header-rows: 1

   * - Feature
     - Stability
     - Performance
     - Notes
   * - :ref:`extended-refs<mkfs-feature-extended-refs>`
     - :statusok:`OK`
     - OK
     -
   * - :ref:`skinny-metadata<mkfs-feature-skinny-metadata>`
     - :statusok:`OK`
     - OK
     -
   * - :ref:`no-holes<mkfs-feature-no-holes>`
     - :statusok:`OK`
     - OK
     -
   * - :ref:`Free space tree<mkfs-feature-free-space-tree>`
     - :statusok:`OK`
     - OK
     -
   * - :ref:`Block group tree<mkfs-feature-block-group-tree>`
     - :statusok:`OK`
     - OK
     -
   * - :ref:`Raid stripe tree<mkfs-feature-raid-stripe-tree>`
     - :statusok:`OK`
     - OK
     -

Interoperability
^^^^^^^^^^^^^^^^

Integration with other Linux features or external systems.
:doc:`See also<Interoperability>`.

.. list-table::
   :header-rows: 1

   * - Feature
     - Stability
     - Performance
     - Notes
   * - :ref:`NFS<interop-nfs>`
     - :statusok:`OK`
     - OK
     -
   * - :ref:`cgroups<interop-cgroups>`
     - :statusok:`OK`
     - OK
     - IO controller
   * - :ref:`io_uring<interop-io-uring>`
     - :statusok:`OK`
     - OK
     -
   * - :ref:`fsverity<interop-fsverity>`
     - :statusok:`OK`
     - OK
     -
   * - :ref:`idmapped mount<interop-idmapped>`
     - :statusok:`OK`
     - OK
     -
   * - :ref:`Samba<interop-samba>`
     - :statusok:`OK`
     - OK
     - compression, server-side copies, snapshots

Please open an issue if:

-  there's a known missing entry
-  a particular feature combination that has a different status and is
   worth mentioning separately
-  you know of a bug that lowers the feature status

.. _status-subpage-block-size:

Subpage block size
------------------

Most commonly used page sizes are 4KiB, 16KiB and 64KiB. All combinations with
a 4KiB sector size filesystems are supported. Some features are not compatible
with subpage or require another feature to work:

.. list-table::
   :header-rows: 1

   * - Feature
     - Status
     - Notes
   * - Inline files
     - :statusunsupp:`unsupported`
     - The max_inline mount option value is ignored, as if mounted with max_inline=0
   * - Free space cache v1
     - :statusunsupp:`unsupported`
     - Free space tree is mandatory, v1 makes some assumptions about page size
   * - Compression
     - :statusok:`partial support`
     - Only page-aligned ranges can be compressed
   * - Sectorsize
     - :statusok:`supported`
     - The list of supported sector sizes on a given version can be found
       in file :file:`/sys/fs/btrfs/features/supported_sectorsizes`


Zoned mode
----------

Features that completely incompatible with zoned mode are listed below.
Compatible features may not be listed and are assumed to work as they
are unaffected by the zoned device constraints.

.. list-table::
   :header-rows: 1

   * - Feature
     - Status
     - Notes
   * - Boot
     - :statusincompat:`incompatible`
     - The blocks where partition table is stored are used for super block
   * - Mixed block groups
     - :statusincompat:`incompatible`
     - Interleaving data and metadata would lead to out of order write
   * - NODATACOW
     - :statusincompat:`incompatible`
     - In-place overwrite
   * - fallocate
     - :statusincompat:`incompatible`
     - Preallocation of blocks would require an out of order write
   * - Free space cache v1
     - :statusincompat:`incompatible`
     - Cache data are updated in a NODATACOW-way
   * - Swapfile
     - :statusincompat:`incompatible`
     - Swap blocks are written out of order
   * - Offline UUID change
     - :statusincompat:`incompatible`
     - Metadata blocks are updated in-place
   * - Free space tree
     - :statusok:`supported`
     -
   * - Block group tree
     - :statusok:`supported`
     -
   * - Raid stripe tree
     - :statusok:`supported`
     - Allows to use RAID in zoned mode
   * - Filesystem resize
     - :statusok:`supported`
     -
   * - Balance
     - :statusok:`supported`
     -
   * - Metadata UUID change
     - :statusok:`supported`
     -
   * - RAID0, RAID1*
     - :statusok:`supported`
     - requires `raid-stripe-tree`
   * - RAID56
     - not implemented
     - will be supported once raid-stripe-tree support is implemented
   * - discard
     - not implemented
     - May not be required at all due to automatic zone reclaim
   * - fsverity
     - TBD
     -
   * - seeding
     - TBD
     -


Details that do not fit the table
---------------------------------

Defrag
^^^^^^

The data affected by the defragmentation process will be newly written
and will consume new space, the links to the original extents will not
be kept. See also :doc:`btrfs-filesystem` . Though
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

-  layout of the main data structures, e.g. superblock, b-tree nodes,
   b-tree keys, block headers
-  the COW mechanism, based on the original design of Ohad Rodeh's paper
   "B-trees, Shadowing and Clones" (http://sylab-srv.cs.fiu.edu/lib/exe/fetch.php?media=paperclub:shadow_btree.pdf)

Newly introduced features build on top of the above and could add
specific structures. If a backward compatibility is not possible to
maintain, a bit in the filesystem superblock denotes that and the level
of incompatibility (full, read-only mount possible).
