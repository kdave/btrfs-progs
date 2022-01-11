btrfs-man5(5)
=============

DESCRIPTION
-----------

This document describes topics related to BTRFS that are not specific to the
tools.  Currently covers:

#. mount options
#. filesystem features
#. checksum algorithms
#. compression
#. filesystem exclusive operations
#. filesystem limits
#. bootloader support
#. file attributes
#. zoned mode
#. control device
#. filesystems with multiple block group profiles
#. seeding device
#. raid56 status and recommended practices
#. storage model
#. hardware considerations


MOUNT OPTIONS
-------------

.. include:: ch-mount-options.rst

FILESYSTEM FEATURES
-------------------

The basic set of filesystem features gets extended over time. The backward
compatibility is maintained and the features are optional, need to be
explicitly asked for so accidental use will not create incompatibilities.

There are several classes and the respective tools to manage the features:

at mkfs time only
        This is namely for core structures, like the b-tree nodesize or checksum
        algorithm, see ``mkfs.btrfs(8)`` for more details.

after mkfs, on an unmounted filesystem::
        Features that may optimize internal structures or add new structures to support
        new functionality, see ``btrfstune(8)``. The command **btrfs inspect-internal
        dump-super /dev/sdx** will dump a superblock, you can map the value of
        *incompat_flags* to the features listed below

after mkfs, on a mounted filesystem
        The features of a filesystem (with a given UUID) are listed in
        */sys/fs/btrfs/UUID/features/*, one file per feature. The status is stored
        inside the file. The value *1* is for enabled and active, while *0* means the
        feature was enabled at mount time but turned off afterwards.

        Whether a particular feature can be turned on a mounted filesystem can be found
        in the directory */sys/fs/btrfs/features/*, one file per feature. The value *1*
        means the feature can be enabled.

List of features (see also ``mkfs.btrfs(8)`` section *FILESYSTEM FEATURES*):

big_metadata
        (since: 3.4)

        the filesystem uses *nodesize* for metadata blocks, this can be bigger than the
        page size

compress_lzo
        (since: 2.6.38)

        the *lzo* compression has been used on the filesystem, either as a mount option
        or via **btrfs filesystem defrag**.

compress_zstd
        (since: 4.14)

        the *zstd* compression has been used on the filesystem, either as a mount option
        or via **btrfs filesystem defrag**.

default_subvol
        (since: 2.6.34)

        the default subvolume has been set on the filesystem

extended_iref
        (since: 3.7)

        increased hardlink limit per file in a directory to 65536, older kernels
        supported a varying number of hardlinks depending on the sum of all file name
        sizes that can be stored into one metadata block

free_space_tree
        (since: 4.5)

        free space representation using a dedicated b-tree, successor of v1 space cache

metadata_uuid
        (since: 5.0)

        the main filesystem UUID is the metadata_uuid, which stores the new UUID only
        in the superblock while all metadata blocks still have the UUID set at mkfs
        time, see ``btrfstune(8)`` for more

mixed_backref
        (since: 2.6.31)

        the last major disk format change, improved backreferences, now default

mixed_groups
        (since: 2.6.37)

        mixed data and metadata block groups, ie. the data and metadata are not
        separated and occupy the same block groups, this mode is suitable for small
        volumes as there are no constraints how the remaining space should be used
        (compared to the split mode, where empty metadata space cannot be used for data
        and vice versa)

        on the other hand, the final layout is quite unpredictable and possibly highly
        fragmented, which means worse performance

no_holes
        (since: 3.14)

        improved representation of file extents where holes are not explicitly
        stored as an extent, saves a few percent of metadata if sparse files are used

raid1c34
        (since: 5.5)

        extended RAID1 mode with copies on 3 or 4 devices respectively

raid56
        (since: 3.9)

        the filesystem contains or contained a raid56 profile of block groups

rmdir_subvol
        (since: 4.18)

        indicate that ``rmdir(2)`` syscall can delete an empty subvolume just like an
        ordinary directory. Note that this feature only depends on the kernel version.

skinny_metadata
        (since: 3.10)

        reduced-size metadata for extent references, saves a few percent of metadata

send_stream_version
        (since: 5.10)

        number of the highest supported send stream version

supported_checksums
        (since: 5.5)

        list of checksum algorithms supported by the kernel module, the respective
        modules or built-in implementing the algorithms need to be present to mount
        the filesystem, see *CHECKSUM ALGORITHMS*

supported_sectorsizes
        (since: 5.13)

        list of values that are accepted as sector sizes (**mkfs.btrfs --sectorsize**) by
        the running kernel

supported_rescue_options
        (since: 5.11)

        list of values for the mount option *rescue* that are supported by the running
        kernel, see ``btrfs(5)``

zoned
        (since: 5.12)

        zoned mode is allocation/write friendly to host-managed zoned devices,
        allocation space is partitioned into fixed-size zones that must be updated
        sequentially, see *ZONED MODE*

SWAPFILE SUPPORT
----------------

.. include:: ch-swapfile.rst

CHECKSUM ALGORITHMS
-------------------

.. include:: ch-checksumming.rst

COMPRESSION
-----------

.. include:: ch-compression.rst

FILESYSTEM EXCLUSIVE OPERATIONS
-------------------------------

There are several operations that affect the whole filesystem and cannot be run
in parallel. Attempt to start one while another is running will fail.

Since kernel 5.10 the currently running operation can be obtained from
*/sys/fs/UUID/exclusive_operation* with following values and operations:

* balance
* device add
* device delete
* device replace
* resize
* swapfile activate
* none

Enqueuing is supported for several btrfs subcommands so they can be started
at once and then serialized.


FILESYSTEM LIMITS
-----------------

maximum file name length
        255

maximum symlink target length
        depends on the *nodesize* value, for 4KiB it's 3949 bytes, for larger nodesize
        it's 4095 due to the system limit PATH_MAX

        The symlink target may not be a valid path, ie. the path name components
        can exceed the limits (NAME_MAX), there's no content validation at ``symlink(3)``
        creation.

maximum number of inodes
        2^64^ but depends on the available metadata space as the inodes are created
        dynamically

inode numbers
        minimum number: 256 (for subvolumes), regular files and directories: 257

maximum file length
        inherent limit of btrfs is 2^64^ (16 EiB) but the linux VFS limit is 2^63^ (8 EiB)

maximum number of subvolumes
        the subvolume ids can go up to 2^64^ but the number of actual subvolumes
        depends on the available metadata space, the space consumed by all subvolume
        metadata includes bookkeeping of shared extents can be large (MiB, GiB)

maximum number of hardlinks of a file in a directory
        65536 when the *extref* feature is turned on during mkfs (default), roughly
        100 otherwise

minimum filesystem size
        the minimal size of each device depends on the *mixed-bg* feature, without that
        (the default) it's about 109MiB, with mixed-bg it's is 16MiB


BOOTLOADER SUPPORT
------------------

GRUB2 (https://www.gnu.org/software/grub) has the most advanced support of
booting from BTRFS with respect to features.

U-boot (https://www.denx.de/wiki/U-Boot/) has decent support for booting but
not all BTRFS features are implemented, check the documentation.

EXTLINUX (from the https://syslinux.org project) can boot but does not support
all features. Please check the upstream documentation before you use it.

The first 1MiB on each device is unused with the exception of primary
superblock that is on the offset 64KiB and spans 4KiB.


FILE ATTRIBUTES
---------------

The btrfs filesystem supports setting file attributes or flags. Note there are
old and new interfaces, with confusing names. The following list should clarify
that:

* *attributes*: ``chattr(1)`` or ``lsattr(1)`` utilities (the ioctls are
  FS_IOC_GETFLAGS and FS_IOC_SETFLAGS), due to the ioctl names the attributes
  are also called flags
* *xflags*: to distinguish from the previous, it's extended flags, with tunable
  bits similar to the attributes but extensible and new bits will be added in
  the future (the ioctls are FS_IOC_FSGETXATTR and FS_IOC_FSSETXATTR but they
  are not related to extended attributes that are also called xattrs), there's
  no standard tool to change the bits, there's support in ``xfs_io(8)`` as
  command **xfs_io -c chattr**

ATTRIBUTES
^^^^^^^^^^

a
        *append only*, new writes are always written at the end of the file

A
        *no atime updates*

c
        *compress data*, all data written after this attribute is set will be compressed.
        Please note that compression is also affected by the mount options or the parent
        directory attributes.

        When set on a directory, all newly created files will inherit this attribute.
        This attribute cannot be set with 'm' at the same time.

C
        *no copy-on-write*, file data modifications are done in-place

        When set on a directory, all newly created files will inherit this attribute.

        .. note::
                Due to implementation limitations, this flag can be set/unset only on
                empty files.

d
        *no dump*, makes sense with 3rd party tools like ``dump(8)``, on BTRFS the
        attribute can be set/unset but no other special handling is done

D
        *synchronous directory updates*, for more details search ``open(2)`` for *O_SYNC*
        and *O_DSYNC*

i
        *immutable*, no file data and metadata changes allowed even to the root user as
        long as this attribute is set (obviously the exception is unsetting the attribute)

m
        *no compression*, permanently turn off compression on the given file. Any
        compression mount options will not affect this file. (``chattr`` support added in
        1.46.2)

        When set on a directory, all newly created files will inherit this attribute.
        This attribute cannot be set with *c* at the same time.

S
        *synchronous updates*, for more details search ``open(2)`` for *O_SYNC* and
        *O_DSYNC*

No other attributes are supported.  For the complete list please refer to the
``chattr(1)`` manual page.

XFLAGS
^^^^^^

There's overlap of letters assigned to the bits with the attributes, this list
refers to what ``xfs_io(8)`` provides:

i
        *immutable*, same as the attribute

a
        *append only*, same as the attribute

s
        *synchronous updates*, same as the attribute *S*

A
        *no atime updates*, same as the attribute

d
        *no dump*, same as the attribute


ZONED MODE
----------

.. include:: ch-zoned-intro.rst


CONTROL DEVICE
--------------

There's a character special device */dev/btrfs-control* with major and minor
numbers 10 and 234 (the device can be found under the 'misc' category).

.. code-block:: none

        $ ls -l /dev/btrfs-control
        crw------- 1 root root 10, 234 Jan  1 12:00 /dev/btrfs-control

The device accepts some ioctl calls that can perform following actions on the
filesystem module:

* scan devices for btrfs filesystem (ie. to let multi-device filesystems mount
  automatically) and register them with the kernel module
* similar to scan, but also wait until the device scanning process is finished
  for a given filesystem
* get the supported features (can be also found under */sys/fs/btrfs/features*)

The device is created when btrfs is initialized, either as a module or a
built-in functionality and makes sense only in connection with that. Running
eg. mkfs without the module loaded will not register the device and will
probably warn about that.

In rare cases when the module is loaded but the device is not present (most
likely accidentally deleted), it's possible to recreate it by

.. code-block:: bash

        # mknod --mode=600 /dev/btrfs-control c 10 234

or (since 5.11) by a convenience command

.. code-block:: bash

        # btrfs rescue create-control-device

The control device is not strictly required but the device scanning will not
work and a workaround would need to be used to mount a multi-device filesystem.
The mount option *device* can trigger the device scanning during mount, see
also **btrfs device scan**.


FILESYSTEM WITH MULTIPLE PROFILES
---------------------------------

It is possible that a btrfs filesystem contains multiple block group profiles
of the same type.  This could happen when a profile conversion using balance
filters is interrupted (see ``btrfs-balance(8)``).  Some **btrfs** commands perform
a test to detect this kind of condition and print a warning like this:

.. code-block:: none

        WARNING: Multiple block group profiles detected, see 'man btrfs(5)'.
        WARNING:   Data: single, raid1
        WARNING:   Metadata: single, raid1

The corresponding output of **btrfs filesystem df** might look like:

.. code-block:: none

        WARNING: Multiple block group profiles detected, see 'man btrfs(5)'.
        WARNING:   Data: single, raid1
        WARNING:   Metadata: single, raid1
        Data, RAID1: total=832.00MiB, used=0.00B
        Data, single: total=1.63GiB, used=0.00B
        System, single: total=4.00MiB, used=16.00KiB
        Metadata, single: total=8.00MiB, used=112.00KiB
        Metadata, RAID1: total=64.00MiB, used=32.00KiB
        GlobalReserve, single: total=16.25MiB, used=0.00B

There's more than one line for type *Data* and *Metadata*, while the profiles
are *single* and *RAID1*.

This state of the filesystem OK but most likely needs the user/administrator to
take an action and finish the interrupted tasks. This cannot be easily done
automatically, also the user knows the expected final profiles.

In the example above, the filesystem started as a single device and *single*
block group profile. Then another device was added, followed by balance with
*convert=raid1* but for some reason hasn't finished. Restarting the balance
with *convert=raid1* will continue and end up with filesystem with all block
group profiles *RAID1*.

.. note::
        If you're familiar with balance filters, you can use
        *convert=raid1,profiles=single,soft*, which will take only the unconverted
        *single* profiles and convert them to *raid1*. This may speed up the conversion
        as it would not try to rewrite the already convert *raid1* profiles.

Having just one profile is desired as this also clearly defines the profile of
newly allocated block groups, otherwise this depends on internal allocation
policy. When there are multiple profiles present, the order of selection is
RAID6, RAID5, RAID10, RAID1, RAID0 as long as the device number constraints are
satisfied.

Commands that print the warning were chosen so they're brought to user
attention when the filesystem state is being changed in that regard. This is:
**device add**, **device delete**, **balance cancel**, **balance pause**. Commands
that report space usage: **filesystem df**, **device usage**. The command
**filesystem usage** provides a line in the overall summary:

.. code-block:: none

    Multiple profiles:                 yes (data, metadata)


SEEDING DEVICE
--------------

.. include:: ch-seeding-device.rst

RAID56 STATUS AND RECOMMENDED PRACTICES
---------------------------------------

The RAID56 feature provides striping and parity over several devices, same as
the traditional RAID5/6. There are some implementation and design deficiencies
that make it unreliable for some corner cases and the feature **should not be
used in production, only for evaluation or testing**.  The power failure safety
for metadata with RAID56 is not 100%.

Metadata
^^^^^^^^

Do not use *raid5* nor *raid6* for metadata. Use *raid1* or *raid1c3*
respectively.

The substitute profiles provide the same guarantees against loss of 1 or 2
devices, and in some respect can be an improvement.  Recovering from one
missing device will only need to access the remaining 1st or 2nd copy, that in
general may be stored on some other devices due to the way RAID1 works on
btrfs, unlike on a striped profile (similar to *raid0*) that would need all
devices all the time.

The space allocation pattern and consumption is different (eg. on N devices):
for *raid5* as an example, a 1GiB chunk is reserved on each device, while with
*raid1* there's each 1GiB chunk stored on 2 devices. The consumption of each
1GiB of used metadata is then *N * 1GiB* for vs *2 * 1GiB*. Using *raid1*
is also more convenient for balancing/converting to other profile due to lower
requirement on the available chunk space.

Missing/incomplete support
^^^^^^^^^^^^^^^^^^^^^^^^^^

When RAID56 is on the same filesystem with different raid profiles, the space
reporting is inaccurate, eg. **df**, **btrfs filesystem df** or **btrfs filesystem
usage**. When there's only a one profile per block group type (eg. raid5 for data)
the reporting is accurate.

When scrub is started on a RAID56 filesystem, it's started on all devices that
degrade the performance. The workaround is to start it on each device
separately. Due to that the device stats may not match the actual state and
some errors might get reported multiple times.

The *write hole* problem.


STORAGE MODEL
-------------

*A storage model is a model that captures key physical aspects of data
structure in a data store. A filesystem is the logical structure organizing
data on top of the storage device.*

The filesystem assumes several features or limitations of the storage device
and utilizes them or applies measures to guarantee reliability. BTRFS in
particular is based on a COW (copy on write) mode of writing, ie. not updating
data in place but rather writing a new copy to a different location and then
atomically switching the pointers.

In an ideal world, the device does what it promises. The filesystem assumes
that this may not be true so additional mechanisms are applied to either detect
misbehaving hardware or get valid data by other means. The devices may (and do)
apply their own detection and repair mechanisms but we won't assume any.

The following assumptions about storage devices are considered (sorted by
importance, numbers are for further reference):

1. atomicity of reads and writes of blocks/sectors (the smallest unit of data
   the device presents to the upper layers)
2. there's a flush command that instructs the device to forcibly order writes
   before and after the command; alternatively there's a barrier command that
   facilitates the ordering but may not flush the data
3. data sent to write to a given device offset will be written without further
   changes to the data and to the offset
4. writes can be reordered by the device, unless explicitly serialized by the
   flush command
5. reads and writes can be freely reordered and interleaved

The consistency model of BTRFS builds on these assumptions. The logical data
updates are grouped, into a generation, written on the device, serialized by
the flush command and then the super block is written ending the generation.
All logical links among metadata comprising a consistent view of the data may
not cross the generation boundary.

WHEN THINGS GO WRONG
^^^^^^^^^^^^^^^^^^^^

**No or partial atomicity of block reads/writes (1)**

- *Problem*: a partial block contents is written (*torn write*), eg. due to a
  power glitch or other electronics failure during the read/write
- *Detection*: checksum mismatch on read
- *Repair*: use another copy or rebuild from multiple blocks using some encoding
  scheme

**The flush command does not flush (2)**

This is perhaps the most serious problem and impossible to mitigate by
filesystem without limitations and design restrictions. What could happen in
the worst case is that writes from one generation bleed to another one, while
still letting the filesystem consider the generations isolated. Crash at any
point would leave data on the device in an inconsistent state without any hint
what exactly got written, what is missing and leading to stale metadata link
information.

Devices usually honor the flush command, but for performance reasons may do
internal caching, where the flushed data are not yet persistently stored. A
power failure could lead to a similar scenario as above, although it's less
likely that later writes would be written before the cached ones. This is
beyond what a filesystem can take into account. Devices or controllers are
usually equipped with batteries or capacitors to write the cache contents even
after power is cut. (*Battery backed write cache*)

**Data get silently changed on write (3)**

Such thing should not happen frequently, but still can happen spuriously due
the complex internal workings of devices or physical effects of the storage
media itself.

* *Problem*: while the data are written atomically, the contents get changed
* *Detection*: checksum mismatch on read
* 'Repair*: use another copy or rebuild from multiple blocks using some
  encoding scheme

**Data get silently written to another offset (3)**

This would be another serious problem as the filesystem has no information
when it happens. For that reason the measures have to be done ahead of time.
This problem is also commonly called 'ghost write'.

The metadata blocks have the checksum embedded in the blocks, so a correct
atomic write would not corrupt the checksum. It's likely that after reading
such block the data inside would not be consistent with the rest. To rule that
out there's embedded block number in the metadata block. It's the logical
block number because this is what the logical structure expects and verifies.


HARDWARE CONSIDERATIONS
-----------------------

The following is based on information publicly available, user feedback,
community discussions or bug report analyses. It's not complete and further
research is encouraged when in doubt.

MAIN MEMORY
^^^^^^^^^^^

The data structures and raw data blocks are temporarily stored in computer
memory before they get written to the device. It is critical that memory is
reliable because even simple bit flips can have vast consequences and lead to
damaged structures, not only in the filesystem but in the whole operating
system.

Based on experience in the community, memory bit flips are more common than one
would think. When it happens, it's reported by the tree-checker or by a checksum
mismatch after reading blocks. There are some very obvious instances of bit
flips that happen, e.g. in an ordered sequence of keys in metadata blocks. We can
easily infer from the other data what values get damaged and how. However, fixing
that is not straightforward and would require cross-referencing data from the
entire filesystem to see the scope.

If available, ECC memory should lower the chances of bit flips, but this
type of memory is not available in all cases. A memory test should be performed
in case there's a visible bit flip pattern, though this may not detect a faulty
memory module because the actual load of the system could be the factor making
the problems appear. In recent years attacks on how the memory modules operate
have been demonstrated ('rowhammer') achieving specific bits to be flipped.
While these were targeted, this shows that a series of reads or writes can
affect unrelated parts of memory.

Further reading:

* https://en.wikipedia.org/wiki/Row_hammer

What to do:

* run *memtest*, note that sometimes memory errors happen only when the system
  is under heavy load that the default memtest cannot trigger
* memory errors may appear as filesystem going read-only due to "pre write"
  check, that verify meta data before they get written but fail some basic
  consistency checks

DIRECT MEMORY ACCESS (DMA)
^^^^^^^^^^^^^^^^^^^^^^^^^^

Another class of errors is related to DMA (direct memory access) performed
by device drivers. While this could be considered a software error, the
data transfers that happen without CPU assistance may accidentally corrupt
other pages. Storage devices utilize DMA for performance reasons, the
filesystem structures and data pages are passed back and forth, making
errors possible in case page life time is not properly tracked.

There are lots of quirks (device-specific workarounds) in Linux kernel
drivers (regarding not only DMA) that are added when found. The quirks
may avoid specific errors or disable some features to avoid worse problems.

What to do:

* use up-to-date kernel (recent releases or maintained long term support versions)
* as this may be caused by faulty drivers, keep the systems up-to-date

ROTATIONAL DISKS (HDD)
^^^^^^^^^^^^^^^^^^^^^^

Rotational HDDs typically fail at the level of individual sectors or small clusters.
Read failures are caught on the levels below the filesystem and are returned to
the user as *EIO - Input/output error*. Reading the blocks repeatedly may
return the data eventually, but this is better done by specialized tools and
filesystem takes the result of the lower layers. Rewriting the sectors may
trigger internal remapping but this inevitably leads to data loss.

Disk firmware is technically software but from the filesystem perspective is
part of the hardware. IO requests are processed, and caching or various
other optimizations are performed, which may lead to bugs under high load or
unexpected physical conditions or unsupported use cases.

Disks are connected by cables with two ends, both of which can cause problems
when not attached properly. Data transfers are protected by checksums and the
lower layers try hard to transfer the data correctly or not at all. The errors
from badly-connecting cables may manifest as large amount of failed read or
write requests, or as short error bursts depending on physical conditions.

What to do:

* check **smartctl** for potential issues

SOLID STATE DRIVES (SSD)
^^^^^^^^^^^^^^^^^^^^^^^^

The mechanism of information storage is different from HDDs and this affects
the failure mode as well. The data are stored in cells grouped in large blocks
with limited number of resets and other write constraints. The firmware tries
to avoid unnecessary resets and performs optimizations to maximize the storage
media lifetime. The known techniques are deduplication (blocks with same
fingerprint/hash are mapped to same physical block), compression or internal
remapping and garbage collection of used memory cells. Due to the additional
processing there are measures to verity the data e.g. by ECC codes.

The observations of failing SSDs show that the whole electronic fails at once
or affects a lot of data (eg. stored on one chip). Recovering such data
may need specialized equipment and reading data repeatedly does not help as
it's possible with HDDs.

There are several technologies of the memory cells with different
characteristics and price. The lifetime is directly affected by the type and
frequency of data written.  Writing "too much" distinct data (e.g. encrypted)
may render the internal deduplication ineffective and lead to a lot of rewrites
and increased wear of the memory cells.

There are several technologies and manufacturers so it's hard to describe them
but there are some that exhibit similar behaviour:

* expensive SSD will use more durable memory cells and is optimized for
  reliability and high load
* cheap SSD is projected for a lower load ("desktop user") and is optimized for
  cost, it may employ the optimizations and/or extended error reporting
  partially or not at all

It's not possible to reliably determine the expected lifetime of an SSD due to
lack of information about how it works or due to lack of reliable stats provided
by the device.

Metadata writes tend to be the biggest component of lifetime writes to a SSD,
so there is some value in reducing them. Depending on the device class (high
end/low end) the features like DUP block group profiles may affect the
reliability in both ways:

* *high end* are typically more reliable and using 'single' for data and
  metadata could be suitable to reduce device wear
* *low end* could lack ability to identify errors so an additional redundancy
  at the filesystem level (checksums, *DUP*) could help

Only users who consume 50 to 100% of the SSD's actual lifetime writes need to be
concerned by the write amplification of btrfs DUP metadata. Most users will be
far below 50% of the actual lifetime, or will write the drive to death and
discover how many writes 100% of the actual lifetime was. SSD firmware often
adds its own write multipliers that can be arbitrary and unpredictable and
dependent on application behavior, and these will typically have far greater
effect on SSD lifespan than DUP metadata. It's more or less impossible to
predict when a SSD will run out of lifetime writes to within a factor of two, so
it's hard to justify wear reduction as a benefit.

Further reading:

* https://www.snia.org/educational-library/ssd-and-deduplication-end-spinning-disk-2012
* https://www.snia.org/educational-library/realities-solid-state-storage-2013-2013
* https://www.snia.org/educational-library/ssd-performance-primer-2013
* https://www.snia.org/educational-library/how-controllers-maximize-ssd-life-2013

What to do:

* run **smartctl** or self-tests to look for potential issues
* keep the firmware up-to-date

NVM EXPRESS, NON-VOLATILE MEMORY (NVMe)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

NVMe is a type of persistent memory usually connected over a system bus (PCIe)
or similar interface and the speeds are an order of magnitude faster than SSD.
It is also a non-rotating type of storage, and is not typically connected by a
cable. It's not a SCSI type device either but rather a complete specification
for logical device interface.

In a way the errors could be compared to a combination of SSD class and regular
memory. Errors may exhibit as random bit flips or IO failures. There are tools
to access the internal log (**nvme log** and **nvme-cli**) for a more detailed
analysis.

There are separate error detection and correction steps performed e.g. on the
bus level and in most cases never making in to the filesystem level. Once this
happens it could mean there's some systematic error like overheating or bad
physical connection of the device. You may want to run self-tests (using
**smartctl**).

* https://en.wikipedia.org/wiki/NVM_Express
* https://www.smartmontools.org/wiki/NVMe_Support

DRIVE FIRMWARE
^^^^^^^^^^^^^^

Firmware is technically still software but embedded into the hardware. As all
software has bugs, so does firmware. Storage devices can update the firmware
and fix known bugs. In some cases the it's possible to avoid certain bugs by
quirks (device-specific workarounds) in Linux kernel.

A faulty firmware can cause wide range of corruptions from small and localized
to large affecting lots of data. Self-repair capabilities may not be sufficient.

What to do:

* check for firmware updates in case there are known problems, note that
  updating firmware can be risky on itself
* use up-to-date kernel (recent releases or maintained long term support versions)

SD FLASH CARDS
^^^^^^^^^^^^^^

There are a lot of devices with low power consumption and thus using storage
media based on low power consumption too, typically flash memory stored on
a chip enclosed in a detachable card package. An improperly inserted card may be
damaged by electrical spikes when the device is turned on or off. The chips
storing data in turn may be damaged permanently. All types of flash memory
have a limited number of rewrites, so the data are internally translated by FTL
(flash translation layer). This is implemented in firmware (technically a
software) and prone to bugs that manifest as hardware errors.

Adding redundancy like using DUP profiles for both data and metadata can help
in some cases but a full backup might be the best option once problems appear
and replacing the card could be required as well.

HARDWARE AS THE MAIN SOURCE OF FILESYSTEM CORRUPTIONS
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**If you use unreliable hardware and don't know about that, don't blame the
filesystem when it tells you.**


SEE ALSO
--------

``acl(5)``,
``btrfs(8)``,
``chattr(1)``,
``fstrim(8)``,
``ioctl(2)``,
``mkfs.btrfs(8)``,
``mount(8)``,
``swapon(8)``
