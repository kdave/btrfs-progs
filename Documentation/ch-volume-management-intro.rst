BTRFS filesystem can be created on top of single or multiple block devices.
Devices can be then added, removed or replaced on demand.  Data and metadata are
organized in allocation profiles with various redundancy policies.  There's some
similarity with traditional RAID levels, but this could be confusing to users
familiar with the traditional meaning. Due to the similarity, the RAID
terminology is widely used in the documentation.  See ``mkfs.btrfs(8)`` for more
details and the exact profile capabilities and constraints.

The device management works on a mounted filesystem. Devices can be added,
removed or replaced, by commands provided by ``btrfs device`` and ``btrfs replace``.

The profiles can be also changed, provided there's enough workspace to do the
conversion, using the ``btrfs balance`` command and namely the filter *convert*.

Type
        The block group profile type is the main distinction of the information stored
        on the block device. User data are called *Data*, the internal data structures
        managed by filesystem are *Metadata* and *System*.

Profile
        A profile describes an allocation policy based on the redundancy/replication
        constraints in connection with the number of devices. The profile applies to
        data and metadata block groups separately. Eg. *single*, *RAID1*.

RAID level
        Where applicable, the level refers to a profile that matches constraints of the
        standard RAID levels. At the moment the supported ones are: RAID0, RAID1,
        RAID10, RAID5 and RAID6.

Typical use cases
-----------------

Starting with a single-device filesystem
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Assume we've created a filesystem on a block device */dev/sda* with profile
*single/single* (data/metadata), the device size is 50GiB and we've used the
whole device for the filesystem. The mount point is */mnt*.

The amount of data stored is 16GiB, metadata have allocated 2GiB.

Add new device
""""""""""""""

We want to increase the total size of the filesystem and keep the profiles. The
size of the new device */dev/sdb* is 100GiB.

.. code-block:: bash

        $ btrfs device add /dev/sdb /mnt

The amount of free data space increases by less than 100GiB, some space is
allocated for metadata.

Convert to RAID1
""""""""""""""""

Now we want to increase the redundancy level of both data and metadata, but
we'll do that in steps. Note, that the device sizes are not equal and we'll use
that to show the capabilities of split data/metadata and independent profiles.

The constraint for RAID1 gives us at most 50GiB of usable space and exactly 2
copies will be stored on the devices.

First we'll convert the metadata. As the metadata occupy less than 50GiB and
there's enough workspace for the conversion process, we can do:

.. code-block:: bash

        $ btrfs balance start -mconvert=raid1 /mnt

This operation can take a while, because all metadata have to be moved and all
block pointers updated. Depending on the physical locations of the old and new
blocks, the disk seeking is the key factor affecting performance.

You'll note that the system block group has been also converted to RAID1, this
normally happens as the system block group also holds metadata (the physical to
logical mappings).

What changed:

* available data space decreased by 3GiB, usable roughly (50 - 3) + (100 - 3) = 144 GiB
* metadata redundancy increased

IOW, the unequal device sizes allow for combined space for data yet improved
redundancy for metadata. If we decide to increase redundancy of data as well,
we're going to lose 50GiB of the second device for obvious reasons.

.. code-block:: bash

        $ btrfs balance start -dconvert=raid1 /mnt

The balance process needs some workspace (ie. a free device space without any
data or metadata block groups) so the command could fail if there's too much
data or the block groups occupy the whole first device.

The device size of */dev/sdb* as seen by the filesystem remains unchanged, but
the logical space from 50-100GiB will be unused.

Remove device
"""""""""""""

Device removal must satisfy the profile constraints, otherwise the command
fails. For example:

.. code-block:: bash

        $ btrfs device remove /dev/sda /mnt
        ERROR: error removing device '/dev/sda': unable to go below two devices on raid1

In order to remove a device, you need to convert the profile in this case:

.. code-block:: bash

        $ btrfs balance start -mconvert=dup -dconvert=single /mnt
        $ btrfs device remove /dev/sda /mnt
