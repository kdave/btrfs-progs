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

*  all devices must have the same zone size
*  maximum zone size is 8GiB
*  minimum zone size is 4MiB
*  mixing zoned and non-zoned devices is possible, the zone writes are emulated,
   but this is namely for testing
*  the super block is handled in a special way and is at different locations than on a non-zoned filesystem:

   *  primary: 0B (and the next two zones)
   *  secondary: 512GiB (and the next two zones)
   *  tertiary: 4TiB (4096GiB, and the next two zones)

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

* only single (data, metadata) and DUP (metadata) profile is supported
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

Zone reclaim, garbage collection
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

As the zones are append-only, overwriting data or COW changes in metadata
make parts of the zones used but not connected to the filesystem structures.
This makes the space unusable and grows over time. Once the ratio hits a
(configurable) threshold a background reclaim process is started and relocates
the remaining blocks in use to a new zone. The old one is reset and can be used
again.

This process may take some time depending on other background work or
amount of new data written. It is possible to hit an intermittent ENOSPC.
Some devices also limit number of active zones.

Devices
^^^^^^^

Real hardware
"""""""""""""

The WD Ultrastar series 600 advertises HM-SMR, i.e. the host-managed zoned
mode. There are two more: DA (device managed, no zoned information exported to
the system), HA (host aware, can be used as regular disk but zoned writes
improve performance). There are not many devices available at the moment, the
information about exact zoned mode is hard to find, check data sheets or
community sources gathering information from real devices.

Note: zoned mode won't work with DM-SMR disks.

-  Ultrastar® DC ZN540 NVMe ZNS SSD (`product
   brief <https://documents.westerndigital.com/content/dam/doc-library/en_us/assets/public/western-digital/collateral/product-brief/product-brief-ultrastar-dc-zn540.pdf>`__)

Emulated: null_blk
""""""""""""""""""

The driver *null_blk* provides memory backed device and is suitable for
testing. There are some quirks setting up the devices. The module must be
loaded with *nr_devices=0* or the numbering of device nodes will be offset. The
*configfs* must be mounted at */sys/kernel/config* and the administration of
the null_blk devices is done in */sys/kernel/config/nullb*. The device nodes
are named like :file:`/dev/nullb0` and are numbered sequentially. NOTE: the device
name may be different than the named directory in sysfs!

Setup:

.. code-block:: bash

   modprobe configfs
   modprobe null_blk nr_devices=0

Create a device *mydev*, assuming no other previously created devices, size is
2048MiB, zone size 256MiB. There are more tunable parameters, this is a minimal
example taking defaults:

.. code-block:: bash

        cd /sys/kernel/config/nullb/
        mkdir mydev
        cd mydev
        echo 2048 > size
        echo 1 > zoned
        echo 1 > memory_backed
        echo 256 > zone_size
        echo 1 > power

This will create a device :file:`/dev/nullb0` and the value of file *index* will
match the ending number of the device node.

Remove the device:

.. code-block:: bash

   rmdir /sys/kernel/config/nullb/mydev

Then continue with :command:`mkfs.btrfs /dev/nullb0`, the zoned mode is auto-detected.

For convenience, there's a script wrapping the basic null_blk management operations
https://github.com/kdave/nullb.git, the above commands become:

.. code-block:: bash

   nullb setup
   nullb create -s 2g -z 256
   mkfs.btrfs /dev/nullb0
   ...
   nullb rm nullb0

Emulated: TCMU runner
"""""""""""""""""""""

TCMU is a framework to emulate SCSI devices in userspace, providing various
backends for the storage, with zoned support as well. A file-backed zoned
device can provide more options for larger storage and zone size. Please follow
the instructions at https://zonedstorage.io/projects/tcmu-runner/ .

Compatibility, incompatibility
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

-  the feature sets an incompat bit and requires new kernel to access the
   filesystem (for both read and write)
-  superblock needs to be handled in a special way, there are still 3 copies
   but at different offsets (0, 512GiB, 4TiB) and the 2 consecutive zones are a
   ring buffer of the superblocks, finding the latest one needs reading it from
   the write pointer or do a full scan of the zones
-  mixing zoned and non zoned devices is possible (zones are emulated) but is
   recommended only for testing
-  mixing zoned devices with different zone sizes is not possible
-  zone sizes must be power of two, zone sizes of real devices are e.g. 256MiB
   or 1GiB, larger size is expected, maximum zone size supported by btrfs is
   8GiB

Status, stability, reporting bugs
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The zoned mode has been released in 5.12 and there are still some rough edges
and corner cases one can hit during testing. Please report bugs to
https://github.com/naota/linux/issues/ .

References
^^^^^^^^^^

-  https://zonedstorage.io

   -  https://zonedstorage.io/projects/libzbc/ -- *libzbc* is library and set
      of tools to directly manipulate devices with ZBC/ZAC support
   -  https://zonedstorage.io/projects/libzbd/ -- *libzbd* uses the kernel
      provided zoned block device interface based on the ioctl() system calls

-  https://hddscan.com/blog/2020/hdd-wd-smr.html -- some details about exact device types
-  https://lwn.net/Articles/853308/ -- *Btrfs on zoned block devices*
-  https://www.usenix.org/conference/vault20/presentation/bjorling -- Zone
   Append: A New Way of Writing to Zoned Storage
