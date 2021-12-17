Trim/discard
============

Trim or discard is an operation on a storage device based on flash technology
(SSD, NVMe or similar), a thin-provisioned device or could be emulated on top
of other block device types. On real hardware, there's a different lifetime
span of the memory cells and the driver firmware usually tries to optimize for
that. The trim operation issued by user provides hints about what data are
unused and allow to reclaim the memory cells. On thin-provisioned or emulated
this is could simply free the space.

There are three main uses of trim that BTRFS supports:

synchronous
        enabled by mounting filesystem with ``-o discard`` or ``-o
        discard=sync``, the trim is done right after the file extents get freed,
        this however could have severe performance hit and is not recommended
        as the ranges to be trimmed could be too fragmented

asynchronous
        enabled by mounting filesystem with ``-o discard=async``, which is an
        improved version of the synchronous trim where the freed file extents
        are first tracked in memory and after a period or enough ranges accumulate
        the trim is started, expecting the ranges to be much larger and
        allowing to throttle the number of IO requests which does not interfere
        with the rest of the filesystem activity

manually by fstrim
        the tool ``fstrim`` starts a trim operation on the whole filesystem, no
        mount options need to be specified, so it's up to the filesystem to
        traverse the free space and start the trim, this is suitable for running
        it as periodic service

The trim is considered only a hint to the device, it could ignore it completely,
start it only on ranges meeting some criteria, or decide not to do it because of
other factors affecting the memory cells. The device itself could internally
relocate the data, however this leads to unexpected performance drop. Running
trim periodically could prevent that too.

When a filesystem is created by ``mkfs.btrfs`` and is capable of trim, then it's
by default performed on all devices.
