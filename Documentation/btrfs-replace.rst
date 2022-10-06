btrfs-replace(8)
================

SYNOPSIS
--------

**btrfs replace** <subcommand> <args>

DESCRIPTION
-----------

**btrfs replace** is used to replace btrfs managed devices with other device.

SUBCOMMAND
----------

cancel <mount_point>
        Cancel a running device replace operation.

start [options] <srcdev>|<devid> <targetdev> <path>
        Replace device of a btrfs filesystem.

        On a live filesystem, duplicate the data to the target device which
        is currently stored on the source device.
        If the source device is not available anymore, or if the -r option is set,
        the data is built only using the RAID redundancy mechanisms.
        After completion of the operation, the source device is removed from the
        filesystem.
        If the *srcdev* is a numerical value, it is assumed to be the device id
        of the filesystem which is mounted at *path*, otherwise it is
        the path to the source device. If the source device is disconnected,
        from the system, you have to use the devid parameter format.
        The *targetdev* needs to be same size or larger than the *srcdev*.

        .. note::
                The filesystem has to be resized to fully take advantage of a
                larger target device; this can be achieved with
                ``btrfs filesystem resize <devid>:max /path``

        ``Options``

        -r
                only read from *srcdev* if no other zero-defect mirror exists.
                (enable this if your drive has lots of read errors, the access would be very
                slow)
        -f
                force using and overwriting *targetdev* even if it looks like
                it contains a valid btrfs filesystem.

                A valid filesystem is assumed if a btrfs superblock is found which contains a
                correct checksum. Devices that are currently mounted are
                never allowed to be used as the *targetdev*.
        -B
                no background replace.
        --enqueue
                wait if there's another exclusive operation running, otherwise continue

        -K|--nodiscard
                Do not perform whole device TRIM operation on devices that are capable of that.
                This does not affect discard/trim operation when the filesystem is mounted.
                Please see the mount option *discard* for that in :doc:`btrfs(5)<btrfs-man5>`.

status [-1] <mount_point>
        Print status and progress information of a running device replace operation.

        ``Options``

        -1
                print once instead of print continuously until the replace
                operation finishes (or is cancelled)


EXAMPLES
--------

Replacing an online drive with a bigger one
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Given the following filesystem mounted at `/mnt/my-vault`


.. code-block:: none

        Label: 'MyVault'  uuid: ae20903e-b72d-49ba-b944-901fc6d888a1
                Total devices 2 FS bytes used 1TiB
                devid    1 size 1TiB used 500.00GiB path /dev/sda
                devid    2 size 1TiB used 500.00GiB path /dev/sdb

In order to replace */dev/sda* (*devid 1*) with a bigger drive located at
*/dev/sdc* you would run the following:

.. code-block:: bash

        btrfs replace start 1 /dev/sdc /mnt/my-vault/

You can monitor progress via:

.. code-block:: bash

        btrfs replace status /mnt/my-vault/

After the replacement is complete, as per the docs at :doc:`btrfs-filesystem(8)<btrfs-filesystem>` in
order to use the entire storage space of the new drive you need to run:

.. code-block:: bash

        btrfs filesystem resize 1:max /mnt/my-vault/

EXIT STATUS
-----------

**btrfs replace** returns a zero exit status if it succeeds. Non zero is
returned in case of failure.

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.  Please refer to the documentation at
https://btrfs.readthedocs.io or wiki http://btrfs.wiki.kernel.org for further
information.

SEE ALSO
--------

:doc:`btrfs-device(8)<btrfs-device>`,
:doc:`btrfs-filesystem(8)<btrfs-filesystem>`,
:doc:`mkfs.btrfs(8)<mkfs.btrfs>`
