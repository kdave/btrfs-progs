Data and metadata are checksummed by default, the checksum is calculated before
write and verifed after reading the blocks from devices. The whole metadata
block has a checksum stored inline in the b-tree node header, each data block
has a detached checksum stored in the checksum tree.

There are several checksum algorithms supported. The default and backward
compatible is *crc32c*.  Since kernel 5.5 there are three more with different
characteristics and trade-offs regarding speed and strength. The following list
may help you to decide which one to select.

CRC32C (32bit digest)
        default, best backward compatibility, very fast, modern CPUs have
        instruction-level support, not collision-resistant but still good error
        detection capabilities

XXHASH (64bit digest)
        can be used as CRC32C successor, very fast, optimized for modern CPUs utilizing
        instruction pipelining, good collision resistance and error detection

SHA256 (256bit digest)
        a cryptographic-strength hash, relatively slow but with possible CPU
        instruction acceleration or specialized hardware cards, FIPS certified and
        in wide use

BLAKE2b (256bit digest)
        a cryptographic-strength hash, relatively fast with possible CPU acceleration
        using SIMD extensions, not standardized but based on BLAKE which was a SHA3
        finalist, in wide use, the algorithm used is BLAKE2b-256 that's optimized for
        64bit platforms

The *digest size* affects overall size of data block checksums stored in the
filesystem.  The metadata blocks have a fixed area up to 256 bits (32 bytes), so
there's no increase. Each data block has a separate checksum stored, with
additional overhead of the b-tree leaves.

Approximate relative performance of the algorithms, measured against CRC32C
using reference software implementations on a 3.5GHz intel CPU:

========  ============   =======  ================
Digest    Cycles/4KiB    Ratio    Implementation
========  ============   =======  ================
CRC32C            1700      1.00  CPU instruction
XXHASH            2500      1.44  reference impl.
SHA256          105000        61  reference impl.
SHA256           36000        21  libgcrypt/AVX2
SHA256           63000        37  libsodium/AVX2
BLAKE2b          22000        13  reference impl.
BLAKE2b          19000        11  libgcrypt/AVX2
BLAKE2b          19000        11  libsodium/AVX2
========  ============   =======  ================

Many kernels are configured with SHA256 as built-in and not as a module.
The accelerated versions are however provided by the modules and must be loaded
explicitly (**modprobe sha256**) before mounting the filesystem to make use of
them. You can check in */sys/fs/btrfs/FSID/checksum* which one is used. If you
see *sha256-generic*, then you may want to unmount and mount the filesystem
again, changing that on a mounted filesystem is not possible.
Check the file */proc/crypto*, when the implementation is built-in, you'd find

.. code-block:: none

        name         : sha256
        driver       : sha256-generic
        module       : kernel
        priority     : 100
        ...

while accelerated implementation is e.g.

.. code-block:: none

        name         : sha256
        driver       : sha256-avx2
        module       : sha256_ssse3
        priority     : 170
        ...

