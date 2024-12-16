Subpage support
===============

Subpage block size support, or just *subpage* for short, is a feature to allow
using a filesystem that has different size of data block size
(*blocksize*, previously called *sectorsize*)
and the host CPU page size. For easier implementation the support was limited
to the exactly same size of the block and page. On x86_64 this is typically
4KiB, but there are other architectures commonly used that make use of larger
pages, like 64KiB on 64bit ARM or PowerPC or 16KiB on Apple Silicon. This means
filesystems created with 64KiB block size cannot be mounted on a system with
4KiB page size.

Since btrfs-progs 6.7, filesystems are created with a 4KiB block size by
default, though it remains possible to create filesystems with other block sizes
(such as 64KiB with the "-s 64k" option for :command:`mkfs.btrfs`). This
ensures that new filesystems are compatible across other architecture variants
using larger page sizes.

Requirements, limitations
-------------------------

The initial subpage support has been added in kernel 5.15. Most features are
already working without problems. On a 64KiB page system, a filesystem with
4KiB blocksize can be mounted and used as long as the initial mount succeeds.
Subpage support is used by default for systems with a non-4KiB page size since
btrfs-progs 6.7.

Please refer to status page of :ref:`status-subpage-block-size` for
compatibility.
