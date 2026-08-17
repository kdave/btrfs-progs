# TODO

## Stability fixes

* audit error handling of `pwrite/pread`, transaction commits and general error propagation

* verify strings used for paths only using path_* API, no strcpy/strcat/sprintf and the n-variants

* size should use unsigned/size_t

## Core

* finish search API

* use search v2 (conditionally compiled, verify correctness)

* review error level of messages

* add missing error messages

* review information in error messages, add missing numbers, information or clarify if needed

* image dump format v2, finish and move out of experimental

* do quick heuristic during defrag, either by file type, use libmagic/file

* FUSE module, read-only first, read-write optional

* add more injection points, use that in tests for specific critical points

* provide/decide low level API infrastructure for error messages and assertions, e.g. array is checking for NULL but this could silently fail for legitimate use (#1128)

## User interface

* forced chunk allocation; do safety checks

* `fi du` with compression, like compsize

* (not sure) file extent map, something like `filefrag` or xfs\_io fiemap, including compressed extents and device layout (RAID levels);

* merge `btrfstune` to main tool, needs the subcommand hierarchy defined

* add `inspect-internal` command to scan filesystem for incompat features (compression, trees, block group types)

* (not sure) file deduplication, basic support, among a set of a few files, not to  duplicate duperemove

* advanced inspection, e.g. device span on the physical device with offsets

* optional user:group map for send/receive

* `subvol delete sync` add options to trigger sync for each or until the last is cleaned

* more verbose resize, print physical offsets of occupied blocks

* resize also the underlying block device

* add benchmark group, with compression, checksums, maybe heuristic

* option to explain balance filters

* `send/receive` use file descriptor with KTLS (pipe, socket)

* merge other standalone tools to main tool, `select-super` to rescue?, `map-logical` to inspect?

* filesystem resize with automatic resize of the underlying device if applicable

* replace with a smaller device (needs kernel support)

## mkfs.btrfs

* add options for more fine grained device specification, like size, slack

* integrate verity records (issue NNN)

## btrfs-convert

* convert from xfs

## Tests

* add and verify test assumptions that are not covered in general, e.g. feature support

* add tests for internal APIs to the extent that is used, and more

* add a common test result output (TAP, xunit)

## User documentation

* add examples to the end of each page, show most common commands, or show some interesting combinations

## Developer documentation

* internal APIs, describe purpose and scope, decide: docs/headers/auto-sync

* image dump format, v1, v2

## 3rd party tool integration

* `strace` can print the ioctl structures, update upstream

* review status of bootloaders, add `systemd-boot` or others that support btrfs

## Other, new, unsorted

*Unless there's a good place to put a new entry, add it here. Link any related issue \#123.*
