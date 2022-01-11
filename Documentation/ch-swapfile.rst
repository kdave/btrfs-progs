A swapfile is file-backed memory that the system uses to temporarily offload
the RAM.  It is supported since kernel 5.0. Use ``swapon(8)`` to activate the
swapfile. There are some limitations of the implementation in BTRFS and linux
swap subsystem:

* filesystem - must be only single device
* filesystem - must have only *single* data profile
* swapfile - the containing subvolume cannot be snapshotted
* swapfile - must be preallocated
* swapfile - must be nodatacow (ie. also nodatasum)
* swapfile - must not be compressed

The limitations come namely from the COW-based design and mapping layer of
blocks that allows the advanced features like relocation and multi-device
filesystems. However, the swap subsystem expects simpler mapping and no
background changes of the file blocks once they've been attached to swap.

With active swapfiles, the following whole-filesystem operations will skip
swapfile extents or may fail:

* balance - block groups with swapfile extents are skipped and reported, the
  rest will be processed normally
* resize grow - unaffected
* resize shrink - works as long as the extents are outside of the shrunk range
* device add - a new device does not interfere with existing swapfile and this
  operation will work, though no new swapfile can be activated afterwards
* device delete - if the device has been added as above, it can be also deleted
* device replace - ditto

When there are no active swapfiles and a whole-filesystem exclusive operation
is running (eg. balance, device delete, shrink), the swapfiles cannot be
temporarily activated. The operation must finish first.

To create and activate a swapfile run the following commands:

.. code-block:: bash

        # truncate -s 0 swapfile
        # chattr +C swapfile
        # fallocate -l 2G swapfile
        # chmod 0600 swapfile
        # mkswap swapfile
        # swapon swapfile

Please note that the UUID returned by the *mkswap* utility identifies the swap
"filesystem" and because it's stored in a file, it's not generally visible and
usable as an identifier unlike if it was on a block device.

The file will appear in */proc/swaps*:

.. code-block:: none

        # cat /proc/swaps
        Filename          Type          Size           Used      Priority
        /path/swapfile    file          2097152        0         -2
        --------------------

The swapfile can be created as one-time operation or, once properly created,
activated on each boot by the **swapon -a** command (usually started by the
service manager). Add the following entry to */etc/fstab*, assuming the
filesystem that provides the */path* has been already mounted at this point.
Additional mount options relevant for the swapfile can be set too (like
priority, not the BTRFS mount options).

.. code-block:: none

        /path/swapfile        none        swap        defaults      0 0

