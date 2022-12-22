Btrfs supports transparent file compression. There are three algorithms
available: ZLIB, LZO and ZSTD (since v4.14), with various levels.
The compression happens on the level of file extents and the algorithm is
selected by file property, mount option or by a defrag command.
You can have a single btrfs mount point that has some files that are
uncompressed, some that are compressed with LZO, some with ZLIB, for instance
(though you may not want it that way, it is supported).

Once the compression is set, all newly written data will be compressed, i.e.
existing data are untouched. Data are split into smaller chunks (128KiB) before
compression to make random rewrites possible without a high performance hit. Due
to the increased number of extents the metadata consumption is higher. The
chunks are compressed in parallel.

The algorithms can be characterized as follows regarding the speed/ratio
trade-offs:

ZLIB
        * slower, higher compression ratio
        * levels: 1 to 9, mapped directly, default level is 3
        * good backward compatibility
LZO
        * faster compression and decompression than ZLIB, worse compression ratio, designed to be fast
        * no levels
        * good backward compatibility
ZSTD
        * compression comparable to ZLIB with higher compression/decompression speeds and different ratio
        * levels: 1 to 15, mapped directly (higher levels are not available)
        * since 4.14, levels since 5.1

The differences depend on the actual data set and cannot be expressed by a
single number or recommendation. Higher levels consume more CPU time and may
not bring a significant improvement, lower levels are close to real time.

How to enable compression
-------------------------

Typically the compression can be enabled on the whole filesystem, specified for
the mount point. Note that the compression mount options are shared among all
mounts of the same filesystem, either bind mounts or subvolume mounts.
Please refer to section *MOUNT OPTIONS*.

.. code-block:: shell

   $ mount -o compress=zstd /dev/sdx /mnt

This will enable the ``zstd`` algorithm on the default level (which is 3).
The level can be specified manually too like ``zstd:3``. Higher levels compress
better at the cost of time. This in turn may cause increased write latency, low
levels are suitable for real-time compression and on reasonably fast CPU don't
cause noticeable performance drops.

.. code-block:: shell

   $ btrfs filesystem defrag -czstd file

The command above will start defragmentation of the whole *file* and apply
the compression, regardless of the mount option. (Note: specifying level is not
yet implemented). The compression algorithm is not persistent and applies only
to the defragmentation command, for any other writes other compression settings
apply.

Persistent settings on a per-file basis can be set in two ways:

.. code-block:: shell

   $ chattr +c file
   $ btrfs property set file compression zstd

The first command is using legacy interface of file attributes inherited from
ext2 filesystem and is not flexible, so by default the *zlib* compression is
set. The other command sets a property on the file with the given algorithm.
(Note: setting level that way is not yet implemented.)

Compression levels
------------------

The level support of ZLIB has been added in v4.14, LZO does not support levels
(the kernel implementation provides only one), ZSTD level support has been added
in v5.1.

There are 9 levels of ZLIB supported (1 to 9), mapping 1:1 from the mount option
to the algorithm defined level. The default is level 3, which provides the
reasonably good compression ratio and is still reasonably fast. The difference
in compression gain of levels 7, 8 and 9 is comparable but the higher levels
take longer.

The ZSTD support includes levels 1 to 15, a subset of full range of what ZSTD
provides. Levels 1-3 are real-time, 4-8 slower with improved compression and
9-15 try even harder though the resulting size may not be significantly improved.

Level 0 always maps to the default. The compression level does not affect
compatibility.

Incompressible data
-------------------

Files with already compressed data or with data that won't compress well with
the CPU and memory constraints of the kernel implementations are using a simple
decision logic. If the first portion of data being compressed is not smaller
than the original, the compression of the file is disabled -- unless the
filesystem is mounted with *compress-force*. In that case compression will
always be attempted on the file only to be later discarded. This is not optimal
and subject to optimizations and further development.

If a file is identified as incompressible, a flag is set (*NOCOMPRESS*) and it's
sticky. On that file compression won't be performed unless forced. The flag
can be also set by **chattr +m** (since e2fsprogs 1.46.2) or by properties with
value *no* or *none*. Empty value will reset it to the default that's currently
applicable on the mounted filesystem.

There are two ways to detect incompressible data:

* actual compression attempt - data are compressed, if the result is not smaller,
  it's discarded, so this depends on the algorithm and level
* pre-compression heuristics - a quick statistical evaluation on the data is
  performed and based on the result either compression is performed or skipped,
  the NOCOMPRESS bit is not set just by the heuristic, only if the compression
  algorithm does not make an improvement

.. code-block:: shell

   $ lsattr file
   ---------------------m file

Using the forcing compression is not recommended, the heuristics are
supposed to decide that and compression algorithms internally detect
incompressible data too.

Pre-compression heuristics
--------------------------

The heuristics aim to do a few quick statistical tests on the compressed data
in order to avoid probably costly compression that would turn out to be
inefficient. Compression algorithms could have internal detection of
incompressible data too but this leads to more overhead as the compression is
done in another thread and has to write the data anyway. The heuristic is
read-only and can utilize cached memory.

The tests performed based on the following: data sampling, long repeated
pattern detection, byte frequency, Shannon entropy.

Compatibility
-------------

Compression is done using the COW mechanism so it's incompatible with
*nodatacow*. Direct IO works on compressed files but will fall back to buffered
writes and leads to recompression. Currently *nodatasum* and compression don't
work together.

The compression algorithms have been added over time so the version
compatibility should be also considered, together with other tools that may
access the compressed data like bootloaders.
