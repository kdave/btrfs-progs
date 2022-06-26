Adding new device
^^^^^^^^^^^^^^^^^

The unallocated space requirements depend on the selected storage
profiles.  The requirements for the storage profile must be met for the
selected for both data and metadata (e.g. if you have single data and
RAID1 metadata, the stricter RAID1 requirements must be met or the
filesystem may run out of metadata space and go read-only).

Before adding a drive, make sure there is enough unallocated space on
existing drives to create new metadata block groups (for filesystems
over 50GB, this is `1GB * (number_of_devices + 2))`.

If using a striped profile (`raid0`, `raid10`, `raid5`, or `raid6`), then do a
full data balance of all data after adding a drive.  If adding multiple
drives at once, do a full data balance after adding the last one.

.. code-block:: bash

        btrfs balance start -v --full-balance mnt/

If the balance is interrupted, it can be restarted using the *stripes*
filter (i.e. `-dstripes=1..N` where *N* is the previous size of the array
before the new device was added) as long as all devices are the same size.
If the device sizes are different, a specialized userspace balance tool
is required.  The data balance must be completed before adding any new
devices or increasing the size of existing ones.

.. code-block:: bash

        # For going from 4 disk to 5 disks, in Raid 5
        btrfs balance start -v -dstripes=1..4 mnt/

If you are not using a striped profile now, but intend to convert to a
striped profile in the future, always perform a full data balance after
adding drives or replacing existing drives with larger ones.  The stock
*btrfs balance* tool cannot cope with special cases on filesystems with
striped raid profiles, and will paint itself into a corner that will
require custom userspace balancing tools to recover if you try.

To watch one can use the following:

.. code-block:: bash

        watch "btrfs filesystem usage -T mnt/; btrfs balance status mnt/"

Convert RAID1 after mkfs with defaults
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If you forgot to set the block group profile when creating the volume, run the
following command:

.. code-block:: bash

        btrfs balance start -v convert=raid1,soft mnt/

This will convert all remaining profiles that are not yet *raid1*.

Convert data to RAID10 with RAID1C4 for metadata
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If you a have multi device setup, or you'd like to have different profiles on a
single disk, e.g. *RAID10* for data and *RAID1C4* for metadata and system:

.. code-block:: bash

        btrfs balance start -v -mconvert=raid1C4,soft -dconvert=raid10,soft mnt/

Compact under used chunks
^^^^^^^^^^^^^^^^^^^^^^^^^

If the data chunks are not balanced and used only partially, the *usage* filter
can be used to make them more compact:

.. code-block:: bash

        btrfs balance start -v -dusage=10 mnt/

If the percent starts from a small number, like 5 or 10, the chunks will be
processed relatively quickly and will make more space available. Increasing the
percentage can then make more chunks compact by relocating the data.

Chunks utilized up to 50% can be relocated to other chunks while still freeing
the space. With utilization higher than 50% the chunks will be basically only
moved on the devices. The actual chunk layout may help to coalesce the free
space but this is a secondary effect.

.. code-block:: bash

        for USAGE in {10..50..10} do
            btrfs balance start -v -dusage=$USAGE mnt/
        done

Fix incomplete balance
^^^^^^^^^^^^^^^^^^^^^^

If the balance is interrupted (due to reboot or cancelled) during conversion to
RAID1. The following command will skip all RAID1 chunks that have been already
converted and continue with what's left to convert. Note that an interrupted
conversion may leave the last chunk under utilized.

.. code-block:: bash

        btrfs balance start convert=raid1,soft mnt/
