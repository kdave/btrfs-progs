
Adding new device:
*****************************

The unallocated space requirements depend on the selected storage
profiles.  The requirements for the storage profile must be met for the 
selected for both data and metadata (e.g. if you have single data and
raid1 metadata, the stricter raid1 requirements must be met or the
filesystem may run out of metadata space and go read-only).

Before adding a drive, make sure there is enough unallocated space on
existing drives to create new metadata block groups (for filesystems
over 50GB, this is `1GB * (number_of_devices + 2))`.

If using a striped profile (`raid0`, `raid10`, `raid5`, or `raid6`), then do a
full data balance of all data after adding a drive.  If adding multiple
drives at once, do a full data balance after adding the last one.

.. code-block:: bash

        sudo btrfs balance start -v --full-balance $BTRFS_MOUNT

If the balance is interrupted, it can be restarted using the 'stripes'
filter (i.e. `-dstripes=1..$N` where $N is the previous size of the array
before the new device was added) as long as all devices are the same size.
If the devices are different sizes, a specialized userspace balance tool
is required.  The data balance must be completed before adding any new
devices or increasing the size of existing ones.

.. code-block:: bash

        ## For going from 4 disk to 5 disks, in Raid 5
        sudo btrfs balance start -v -dstripes=1..4 $BTRFS_MOUNT

If you are not using a striped profile now, but intend to convert to a
striped profile in the future, always perform a full data balance after
adding drives or replacing existing drives with larger ones.  The stock
btrfs balance tool cannot cope with special cases on filesystems with
striped raid profiles, and will paint itself into a corner that will
require custom userspace balancing tools to recover if you try.

**To watch one can use the following:**

.. code-block:: bash

        sudo watch "btrfs filesystem usage -T $BTRFS_MOUNT; btrfs balance status $BTRFS_MOUNT"


source:

* https://lore.kernel.org/linux-btrfs/YnmItm%2FNW3eUcvsL@hungrycats.org

Convert raid1 after mkfs with defaults:
*****************************************
If forgot to set the raid type creating the volume
        
.. code-block:: bash
                
        btrfs balance start -v convert=raid1,soft  $BTRFS_MOUNT

Convert data to raid10 with raid1C4 for metadata:
***************************************
If you have multi disk setup, or you like to have different profiles on a single disk:

* ``raid10`` for data
* ``raid1C4`` for metadata and system. 

.. code-block:: bash
                
        btrfs balance start -v -mconvert=raid1C4,soft -dconvert=raid10,soft  $BTRFS_MOUNT


Compacts of chunks bellow 50%:
***************************************
If your data chunks are misbalanced, look at how much space is really used in percentage 

.. code-block:: bash
                
        btrfs balance start -v -dusage=10 $BTRFS_MOUNT

and you can feed that to ``-dusage`` in smaller increments starting from 10. 
This will ask btrfs to rebalance all chunks that are not at that threshold (bigger number means more work). 
Rebalancing means chunks under that usage threshold will have their data moved to other chunks so that they 
can be freed up and made available for new allocations (fixing your filesystem full problem).

.. code-block:: bash

        for USAGE in {10..50..10} do
        btrfs balance start -v -dusage=$USAGE $BTRFS_MOUNT
        done

fix incomplete balance:
***********************

If the balance is interrupted ( reboot or stopped) during coverting to raid1.
Only targets non `raid1` chunks.

.. code-block:: bash

        btrfs balance start convert=raid1,soft $BTRFS_MOUNT

source:

* https://lore.kernel.org/linux-btrfs/c00a206b-ac20-9312-498f-6fbf1ffd1295@petezilla.co.uk/
