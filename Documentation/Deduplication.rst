Deduplication
=============

Going by the definition in the context of filesystems, it's a process of
looking up identical data blocks tracked separately and replacing one of the
copies of the data blocks with a shared logical link to the other.  This leads
to data space savings while it may increase metadata consumption in some cases.

There are two main deduplication types:

* **in-band** *(sometimes also called on-line)* -- all newly written data are
  considered for deduplication before writing
* **out-of-band** *(sometimes also called offline)* -- data for deduplication
  have to be actively looked for and deduplicated by the user application

Both have their pros and cons. BTRFS implements **only out-of-band** type.

BTRFS provides the basic building blocks for deduplication allowing other tools
to choose the strategy and scope of the deduplication.  There are multiple
tools that take different approaches to deduplication, offer additional
features or make trade-offs. The following table lists tools that are known to
be up-to-date, maintained and widely used.

.. list-table::
   :header-rows: 1

   * - Name
     - File based
     - Block based
     - Incremental
   * - `BEES <https://github.com/Zygo/bees>`_
     - No
     - Yes
     - Yes
   * - `duperemove <https://github.com/markfasheh/duperemove>`_
     - Yes
     - No
     - Yes

File based deduplication
------------------------

The tool takes a list of files and tries to find duplicates among data only
from these files. This is suitable e.g. for files that originated from the same
base image, source of a reflinked file. Optionally the tool could track a
database of hashes and allow to deduplicate blocks from more files, or use that
for repeated runs and update the database incrementally.

With multiple block copies, the process can be repeated with pairs of copies until
only one copy remains, i.e. all other copies have been replaced with references
to the first.  The details are tool-specific, as there's no general rule
for which copy is the better one to keep that covers all situations.

Block based deduplication
-------------------------

The tool typically scans the filesystem and builds a database of file block
hashes, then finds candidate files and deduplicates the ranges. The hash
database is kept as an ordinary file and can be scaled according to the needs.

As the files change, the hash database may get out of sync and the scan has to
be done repeatedly.

Safety of block comparison
--------------------------

The deduplication inside the filesystem is implemented as an ``ioctl`` that takes
a source file, destination file and the range. The blocks from both files are
compared for exact match before merging to the same range (i.e. there's no
hash based comparison). Pages representing the extents in memory are locked
prior to deduplication and prevent concurrent modification by buffered writes
or mmapped writes. Blocks are compared byte by byte and not using any hash-based
approach, i.e. the existing checksums are not used.

Since kernel 7.2 the raw checksums can be read by an ioctl (with some
exceptions given the file extent types, like compressed or inline). This can
be used as an initial hint to the deduplication tool to identify blocks without
actually reading them.

Limitations, compatibility
--------------------------

Files that are subject to deduplication must have the same status regarding
COW, i.e. both regular COW files with checksums, or both NOCOW, or files that
are COW but don't have checksums (NODATASUM attribute is set).

If the deduplication is in progress on any file in the filesystem, the *send*
operation cannot be started as it relies on the extent layout being unchanged.
