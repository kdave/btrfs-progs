The COW mechanism and multiple devices under one hood enable an interesting
concept, called a seeding device: extending a read-only filesystem on a single
device filesystem with another device that captures all writes. For example
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
read-only, it can be used to seed multiple filesystems, at the same time. The
UUID that is normally attached to a device is automatically changed to a random
UUID on each mount.

Once the seeding device is mounted, it needs the writable device. After adding
it, something like **remount -o remount,rw /path** makes the filesystem at
*/path* ready for use. The simplest use case is to throw away all changes by
unmounting the filesystem when convenient.

Alternatively, deleting the seeding device from the filesystem can turn it into
a normal filesystem, provided that the writable device can also contain all the
data from the seeding device.

The seeding device flag can be cleared again by **btrfstune -f -s 0**, eg.
allowing to update with newer data but please note that this will invalidate
all existing filesystems that use this particular seeding device. This works
for some use cases, not for others, and a forcing flag to the command is
mandatory to avoid accidental mistakes.

Example how to create and use one seeding device:

.. code-block:: bash

        # mkfs.btrfs /dev/sda
        # mount /dev/sda /mnt/mnt1
        # ... fill mnt1 with data
        # umount /mnt/mnt1
        # btrfstune -S 1 /dev/sda
        # mount /dev/sda /mnt/mnt1
        # btrfs device add /dev/sdb /mnt
        # mount -o remount,rw /mnt/mnt1
        # ... /mnt/mnt1 is now writable

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
