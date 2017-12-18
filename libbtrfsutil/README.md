libbtrfsutil
============

libbtrfsutil is a library for managing Btrfs filesystems. It is licensed under
the LGPL. libbtrfsutil provides interfaces for a subset of the operations
offered by the `btrfs` command line utility. It also includes official Python
bindings (Python 3 only).

Development
-----------

The [development process for btrfs-progs](../README.md#development) applies.

libbtrfsutil only includes operations that are done through the filesystem and
ioctl interface, not operations that modify the filesystem directly (e.g., mkfs
or fsck). This is by design but also a legal necessity, as the filesystem
implementation is GPL but libbtrfsutil is LGPL. That is also why the
libbtrfsutil code is a reimplementation of the btrfs-progs code rather than a
refactoring. Be wary of this when porting functionality.

libbtrfsutil is semantically versioned separately from btrfs-progs. It is the
maintainers' responsibility to bump the version as needed (at most once per
release of btrfs-progs).

A few guidelines:

* All interfaces must be documented in `btrfsutil.h` using the kernel-doc style
* Error codes should be specific about what _exactly_ failed
* Functions should have a path and an fd variant whenever possible
* Spell out terms in function names, etc. rather than abbreviating whenever
  possible
* Don't require the Btrfs UAPI headers for any interfaces (e.g., instead of
  directly exposing a type from `linux/btrfs_tree.h`, abstract it away in a
  type specific to `libbtrfsutil`)
* Preserve API and ABI compatability at all times (i.e., we don't want to bump
  the library major version if we don't have to)
* Include Python bindings for all interfaces
* Write tests for all interfaces
