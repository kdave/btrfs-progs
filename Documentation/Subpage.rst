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
"-s 4k" option for mkfs.btrfs) and mount filesystems with 4KiB sectorsize,
allowing us to push 4KiB sectorsize as default sectorsize for all platforms in the
near future.

Requirements, limitations
-------------------------

The initial subpage support has been added in v5.15, although it's still
considered as experimental at the time of writing (v5.18), most features are
already working without problems.

End users can mount filesystems with 4KiB sectorsize and do their usual
workload, while should not notice any obvious change, as long as the initial
mount succeeded (there are cases a mount will be rejected though).

The following features has some limitations for subpage:

- RAID56 support
  This support is already queued for v5.19 cycle.
  Any fs with RAID56 chunks will be rejected at mount time for now.

- Support for page size other than 64KiB
  The support for other page sizes (16KiB, 32KiB and more) are already queued
  for v5.19 cycle.
  Initially the subpage support is only for 64KiB support, but the design makes
  it pretty easy to enable support for other page sizes.

- No inline extent creation
  This is an artificial limit, to prevent mixed inline and regular extents.

  It's possible to create mixed inline and regular extents even with
  non-subpage mount for certain corner cases, it's way easier to create such
  mixed extents for subpage.

  Thus max_inline mount option will be silently ignored for subpage mounts,
  and it always acts as "max_inline=0".

- Compression write is limited to page aligned ranges
  Compression write for subpage is introduced in v5.16, with the limitation
  that only page aligned range can be compressed.
  This limitation is due to how btrfs handles delayed allocation.

- No support for v1 space cache
  V1 space cache is considered deprecated, and we're defaulting to v2 cache
  in btrfs-progs already.
  The old v1 cache has quite some hard coded page size usage, and consider it
  is already deprecated, we force v2 cache for subpage.

- Slightly higher memory usage for scrub
  This is due to how we allocate pages for scrub, and will be fixed in the coming
  releases soon.
