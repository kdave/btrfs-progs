The COW mechanism and multiple devices under one hood enable an interesting
concept, called a seeding device: extending a read-only filesystem on a
device with another device that captures all writes. For example
imagine an immutable golden image of an operating system enhanced with another
device that allows to use the data from the golden image and normal operation.
This idea originated on CD-ROMs with base OS and allowing to use them for live
systems, but this became obsolete. There are technologies providing similar
functionality, like `unionmount <https://en.wikipedia.org/wiki/Union_mount>`_,
`overlayfs <https://en.wikipedia.org/wiki/OverlayFS>`_ or
`qcow2 <https://en.wikipedia.org/wiki/Qcow#qcow2>`_ image snapshot.

The seeding device starts as a normal filesystem, once the contents is ready,
:command:`btrfstune -S 1` is used to flag it as a seeding device. Mounting such device
will not allow any writes, except adding a new device by :command:`btrfs device add`.
Then the filesystem can be remounted as read-write.

Given that the filesystem on the seeding device is always recognized as
read-only, it can be used to seed multiple filesystems from one device at the
same time. The UUID that is normally attached to a device is automatically
changed to a random UUID on each mount.

.. note::

        Before v6.17 kernel, a seed device could have been mounted
        independently along with sprouted filesystems.
	But since 6.17 kernel, a seed device can only be mounted either through
	a sprouted filesystem, or the seed device itself, not both at the same time.

	This is to ensure a block device to have only a single filesystem bound
	to it, so that runtime device missing events can be properly handled.

Once the seeding device is mounted, it needs the writable device. After adding
it, unmounting and mounting with :command:`umount /path; mount /dev/writable
/path` or remounting read-write with :command:`remount -o remount,rw` makes the
filesystem at :file:`/path` ready for use.

.. note::

        There was a known bug with using remount to make the mount writeable:
        remount will leave the filesystem in a state where it is unable to
        clean deleted snapshots, so it will leak space until it is unmounted
        and mounted properly.

	That bug has fixed in 5.11 and newer kernels.

Furthermore, deleting the seeding device from the filesystem can turn it into
a normal filesystem, provided that the writable device can also contain all the
data from the seeding device.

The seeding device flag can be cleared again by :command:`btrfstune -f -S 0`, e.g.
allowing to update with newer data but please note that this will invalidate
all existing filesystems that use this particular seeding device. This works
for some use cases, not for others, and the forcing flag to the command is
mandatory to avoid accidental mistakes.

Example how to create and use one seeding device:

.. code-block:: bash

        # mkfs.btrfs /dev/sda
        # mount /dev/sda /mnt/mnt1
        ... fill mnt1 with data
        # umount /mnt/mnt1

        # btrfstune -S 1 /dev/sda

        # mount /dev/sda /mnt/mnt1
        # btrfs device add /dev/sdb /mnt/mnt1
        # umount /mnt/mnt1
        # mount /dev/sdb /mnt/mnt1
        ... /mnt/mnt1 is now writable

Now :file:`/mnt/mnt1` can be used normally. The device :file:`/dev/sda` can be mounted
again with a another writable device:

.. code-block:: bash

        # mount /dev/sda /mnt/mnt2
        # btrfs device add /dev/sdc /mnt/mnt2
        # umount /mnt/mnt2
        # mount /dev/sdc /mnt/mnt2
        ... /mnt/mnt2 is now writable

The writable device (file:`/dev/sdb`) can be decoupled from the seeding device and
used independently:

.. code-block:: bash

        # btrfs device delete /dev/sda /mnt/mnt1

As the contents originated in the seeding device, it's possible to turn
:file:`/dev/sdb` to a seeding device again and repeat the whole process.

A few things to note:

* it's recommended to use only single device for the seeding device, it works
  for multiple devices but the *single* profile must be used in order to make
  the seeding device deletion work
* block group profiles *single* and *dup* support the use cases above
* the label is copied from the seeding device and can be changed by :command:`btrfs filesystem label`
* each new mount of the seeding device gets a new random UUID
* :command:`umount /path; mount /dev/writable /path` can be replaced with
  :command:`mount -o remount,rw /path`
  but it won't reclaim space of deleted subvolumes until the seeding device
  is mounted read-write again before making it seeding again

Chained seeding devices
^^^^^^^^^^^^^^^^^^^^^^^

Though it's not recommended and is rather an obscure and untested use case,
chaining seeding devices is possible. In the first example, the writable device
:file:`/dev/sdb` can be turned onto another seeding device again, depending on the
unchanged seeding device :file:`/dev/sda`. Then using :file:`/dev/sdb` as the primary
seeding device it can be extended with another writable device, say :file:`/dev/sdd`,
and it continues as before as a simple tree structure on devices.

.. code-block:: bash

        # mkfs.btrfs /dev/sda
        # mount /dev/sda /mnt/mnt1
        ... fill mnt1 with data
        # umount /mnt/mnt1

        # btrfstune -S 1 /dev/sda

        # mount /dev/sda /mnt/mnt1
        # btrfs device add /dev/sdb /mnt/mnt1
        # mount -o remount,rw /mnt/mnt1
        ... /mnt/mnt1 is now writable
        # umount /mnt/mnt1

        # btrfstune -S 1 /dev/sdb

        # mount /dev/sdb /mnt/mnt1
        # btrfs device add /dev/sdc /mnt
        # mount -o remount,rw /mnt/mnt1
        ... /mnt/mnt1 is now writable
        # umount /mnt/mnt1

As a result we have:

* *sda* is a single seeding device, with its initial contents
* *sdb* is a seeding device but requires *sda*, the contents are from the time
  when *sdb* is made seeding, i.e. contents of *sda* with any later changes
* *sdc* last writable, can be made a seeding one the same way as was *sdb*,
  preserving its contents and depending on *sda* and *sdb*

As long as the seeding devices are unmodified and available, they can be used
to start another branch.
