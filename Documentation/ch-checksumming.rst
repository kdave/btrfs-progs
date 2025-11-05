Data and metadata are checksummed by default. The checksum is calculated before
writing and verified after reading the blocks from devices. The whole metadata
block has an inline checksum stored in the b-tree node header. Each data block
has a detached checksum stored in the checksum tree.

.. note::
   Since a data checksum is calculated just before submitting to the block
   device, btrfs has a strong requirement that the corresponding data block must
   not be modified until the writeback is finished.

   This requirement is met for a buffered write as btrfs has the full control on
   its page cache, but a direct write (``O_DIRECT``) bypasses page cache, and
   btrfs can not control the direct IO buffer (as it can be in user space memory).
   Thus it's possible that a user space program modifies its direct write buffer
   before the buffer is fully written back, and this can lead to a data
   checksum mismatch.

   To avoid this, kernel starting with version 6.14 will force a direct
   write to fall back to buffered, if the inode requires a data checksum.
   This will bring a small performance penalty. If you require true zero-copy
   direct writes, then set the ``NODATASUM`` flag for the inode and make
   sure the direct IO buffer is fully aligned to block size.

There are several checksum algorithms supported. The default and backward
compatible algorithm is *crc32c*. Since kernel 5.5 there are three more with different
characteristics and trade-offs regarding speed and strength. The following list
may help you to decide which one to select.

CRC32C (32 bits digest)
        Default, best backward compatibility. Very fast, modern CPUs have
        instruction-level support, not collision-resistant but still good error
        detection capabilities.

XXHASH (64 bits digest)
        Can be used as CRC32C successor. Very fast, optimized for modern CPUs utilizing
        instruction pipelining, good collision resistance and error detection.

SHA256 (256 bits digest)
        Cryptographic-strength hash. Relatively slow but with possible CPU
        instruction acceleration or specialized hardware cards. FIPS certified and
        in wide use.

BLAKE2b (256 bits digest)
        Cryptographic-strength hash. Relatively fast, with possible CPU acceleration
        using SIMD extensions. Not standardized but based on BLAKE which was a SHA3
        finalist, in wide use. The algorithm used is BLAKE2b-256 that's optimized for
        64-bit platforms.

The *digest size* affects overall size of data block checksums stored in the
filesystem.  The metadata blocks have a fixed area up to 256 bits (32 bytes), so
there's no increase. Each data block has a separate checksum stored, with
additional overhead of the b-tree leaves.

Approximate relative performance of the algorithms, measured against CRC32C
using implementations on a 11th gen 3.6GHz intel CPU:

========  ============   =======  ================================
Digest    Cycles/4KiB    Ratio    Implementation
========  ============   =======  ================================
CRC32C             470      1.00  CPU instruction, PCL combination
XXHASH             870       1.9  reference impl.
SHA256            7600        16  libgcrypt
SHA256            8500        18  openssl
SHA256            8700        18  botan
SHA256           32000        68  builtin, CPU instruction
SHA256           37000        78  libsodium
SHA256           78000       166  builtin, reference impl.
BLAKE2b          10000        21  builtin/AVX2
BLAKE2b          10900        23  libgcrypt
BLAKE2b          13500        29  builtin/SSE41
BLAKE2b          13700        29  libsodium
BLAKE2b          14100        30  openssl
BLAKE2b          14500        31  kcapi
BLAKE2b          14500        34  builtin, reference impl.
========  ============   =======  ================================

Many kernels are configured with SHA256 as built-in and not as a module.
Up to kernel v6.15 the accelerated versions are however provided by the
modules and must be loaded
explicitly (:command:`modprobe sha256`) before mounting the filesystem to make use of
them. You can check in :file:`/sys/fs/btrfs/FSID/checksum` which one is used. If you
see *sha256-generic*, then you may want to unmount and mount the filesystem
again. Changing that on a mounted filesystem is not possible.

Since kernel v6.16 the accelereated implementation is always used if available.

Check the file :file:`/proc/crypto`, when the implementation is built-in, you'd find:

.. code-block:: none

        name         : sha256
        driver       : sha256-generic
        module       : kernel
        priority     : 100
        ...

While accelerated implementation is e.g.:

.. code-block:: none

        name         : sha256
        driver       : sha256-avx2
        module       : sha256_ssse3
        priority     : 170
        ...

