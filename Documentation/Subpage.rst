Subpage support
===============

Subpage block size support, or just *subpage* for short, is a feature to allow
using a filesystem that has different size of data block size (*sectorsize*)
and the host CPU page size. For easier implementation the support was limited
to the exactly same size of the block and page. On x86_64 this is typically
4KiB, but there are other architectures commonly used that make use of larger
pages, like 64KiB on 64bit ARM or PowerPC. A filesystem created on one cannot
be mounted on the other.  The subpage support is still work in progress in 5.18
but the support is incrementally added with each release.
