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
in parallel. Attempt to start one while another is running will fail (see
exceptions below).

Since kernel 5.10 the currently running operation can be obtained from
*/sys/fs/UUID/exclusive_operation* with following values and operations:

* balance
* balance paused (since 5.17)
* device add
* device delete
* device replace
* resize
* swapfile activate
* none

Enqueuing is supported for several btrfs subcommands so they can be started
at once and then serialized.

There's an exception when a paused balance allows to start a device add
operation as they don't really collide and this can be used to add more space
for the balance to finish.

FILESYSTEM LIMITS
-----------------

.. include:: ch-fs-limits.rst

BOOTLOADER SUPPORT
------------------

.. include:: ch-bootloaders.rst

FILE ATTRIBUTES
---------------

.. include:: ch-file-attributes.rst

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

.. include:: ch-hardware-considerations.rst

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
