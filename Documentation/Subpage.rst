Subpage support
===============

Subpage block size support, or just *subpage* for short, is a feature to allow
using a filesystem that has different size of data block size (*sectorsize*)
and the host CPU page size. For easier implementation the support was limited
to the exactly same size of the block and page. On x86_64 this is typically
4KiB, but there are other architectures commonly used that make use of larger
pages, like 64KiB on 64bit ARM or PowerPC. This means filesystems created
with 64KiB sector size cannot be mounted on a system with 4KiB page size.

While with subpage support, systems with 64KiB page size can create (still needs
"-s 4k" option for :command:`mkfs.btrfs`) and mount filesystems with 4KiB sectorsize.

Requirements, limitations
-------------------------

The initial subpage support has been added in v5.15, although it's still
considered as experimental, most features are already working without problems.

End users can mount filesystems with 4KiB sectorsize and do their usual
workload, while should not notice any obvious change, as long as the initial
mount succeeded (there are cases a mount will be rejected though).

The following features has some limitations for subpage:

- Supported page sizes: 4KiB, 8KiB, 16KiB, 32KiB, 64KiB

- Supported filesystem sector sizes on a given host are exported in
  :file:`/sys/fs/btrfs/features/supported_sectorsizes`

- No inline extents

  This is an artificial limitation, to prevent mixed inline and regular extents.

  Thus max_inline mount option will be silently ignored for subpage mounts,
  and it always acts as "max_inline=0".

- Compression writes are limited to page aligned ranges

  Compression write for subpage has been introduced in v5.16, with the
  limitation that only page aligned range can be compressed.  This limitation
  is due to how btrfs handles delayed allocation.

- No support for v1 space cache

  The old v1 cache has quite some hard coded page size usage, and considering
  it already deprecated, we force v2 cache for subpage.
