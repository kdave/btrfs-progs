Since version 5.12 btrfs supports so called *zoned mode*. This is a special
on-disk format and allocation/write strategy that's friendly to zoned devices.
In short, a device is partitioned into fixed-size zones and each zone can be
updated by append-only manner, or reset. As btrfs has no fixed data structures,
except the super blocks, the zoned mode only requires block placement that
follows the device constraints. You can learn about the whole architecture at
https://zonedstorage.io .

The devices are also called SMR/ZBC/ZNS, in *host-managed* mode. Note that
there are devices that appear as non-zoned but actually are, this is
*drive-managed* and using zoned mode won't help.

The zone size depends on the device, typical sizes are 256MiB or 1GiB. In
general it must be a power of two. Emulated zoned devices like *null_blk* allow
to set various zone sizes.

Requirements, limitations
^^^^^^^^^^^^^^^^^^^^^^^^^

* all devices must have the same zone size
* maximum zone size is 8GiB
* minimum zone size is 4MiB
* mixing zoned and non-zoned devices is possible, the zone writes are emulated,
  but this is namely for testing
* the super block is handled in a special way and is at different locations than on a non-zoned filesystem:
   * primary: 0B (and the next two zones)
   * secondary: 512GiB (and the next two zones)
   * tertiary: 4TiB (4096GiB, and the next two zones)

Incompatible features
^^^^^^^^^^^^^^^^^^^^^

The main constraint of the zoned devices is lack of in-place update of the data.
This is inherently incompatible with some features:

* NODATACOW - overwrite in-place, cannot create such files
* fallocate - preallocating space for in-place first write
* mixed-bg - unordered writes to data and metadata, fixing that means using
  separate data and metadata block groups
* booting - the zone at offset 0 contains superblock, resetting the zone would
  destroy the bootloader data

Initial support lacks some features but they're planned:

* only single profile is supported
* fstrim - due to dependency on free space cache v1

Super block
^^^^^^^^^^^

As said above, super block is handled in a special way. In order to be crash
safe, at least one zone in a known location must contain a valid superblock.
This is implemented as a ring buffer in two consecutive zones, starting from
known offsets 0B, 512GiB and 4TiB.

The values are different than on non-zoned devices. Each new super block is
appended to the end of the zone, once it's filled, the zone is reset and writes
continue to the next one. Looking up the latest super block needs to read
offsets of both zones and determine the last written version.

The amount of space reserved for super block depends on the zone size. The
secondary and tertiary copies are at distant offsets as the capacity of the
devices is expected to be large, tens of terabytes. Maximum zone size supported
is 8GiB, which would mean that e.g. offset 0-16GiB would be reserved just for
the super block on a hypothetical device of that zone size. This is wasteful
but required to guarantee crash safety.
