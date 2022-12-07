The COW mechanism and multiple devices under one hood enable an interesting
concept, called a seeding device: extending a read-only filesystem on a
device with another device that captures all writes. For example
imagine an immutable golden image of an operating system enhanced with another
device that allows to use the data from the golden image and normal operation.
This idea originated on CD-ROMs with base OS and allowing to use them for live
systems, but this became obsolete. There are technologies providing similar
functionality, like *unionmount*, *overlayfs* or *qcow2* image snapshot.

The seeding device starts as a normal filesystem, once the contents is ready,
**btrfstune -S 1** is used to flag it as a seeding device. Mounting such device
will not allow any writes, except adding a new device by **btrfs device add**.
Then the filesystem can be remounted as read-write.

Given that the filesystem on the seeding device is always recognized as
read-only, it can be used to seed multiple filesystems from one device at the
same time. The UUID that is normally attached to a device is automatically
changed to a random UUID on each mount.

Once the seeding device is mounted, it needs the writable device. After adding
it, something like **remount -o remount,rw /path** makes the filesystem at
*/path* ready for use. The simplest use case is to throw away all changes by
unmounting the filesystem when convenient.

Alternatively, deleting the seeding device from the filesystem can turn it into
a normal filesystem, provided that the writable device can also contain all the
data from the seeding device.

The seeding device flag can be cleared again by **btrfstune -f -S 0**, e.g.
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
        # mount -o remount,rw /mnt/mnt1
        ... /mnt/mnt1 is now writable

Now */mnt/mnt1* can be used normally. The device */dev/sda* can be mounted
again with a another writable device:

.. code-block:: bash

        # mount /dev/sda /mnt/mnt2
        # btrfs device add /dev/sdc /mnt/mnt2
        # mount -o remount,rw /mnt/mnt2
        ... /mnt/mnt2 is now writable

The writable device (*/dev/sdb*) can be decoupled from the seeding device and
used independently:

.. code-block:: bash

        # btrfs device delete /dev/sda /mnt/mnt1

As the contents originated in the seeding device, it's possible to turn
*/dev/sdb* to a seeding device again and repeat the whole process.

A few things to note:

* it's recommended to use only single device for the seeding device, it works
  for multiple devices but the *single* profile must be used in order to make
  the seeding device deletion work
* block group profiles *single* and *dup* support the use cases above
* the label is copied from the seeding device and can be changed by **btrfs filesystem label**
* each new mount of the seeding device gets a new random UUID

Chained seeding devices
^^^^^^^^^^^^^^^^^^^^^^^

Though it's not recommended and is rather an obscure and untested use case,
chaining seeding devices is possible. In the first example, the writable device
*/dev/sdb* can be turned onto another seeding device again, depending on the
unchanged seeding device */dev/sda*. Then using */dev/sdb* as the primary
seeding device it can be extended with another writable device, say */dev/sdd*,
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
