A swapfile, when active, is a file-backed swap area.  It is supported since kernel 5.0.
Use ``swapon(8)`` to activate it, until then (respectively again after deactivating it
with ``swapoff(8)``) it's just a normal file (with NODATACOW set), for which the special
restrictions for active swapfiles don't apply.

There are some limitations of the implementation in BTRFS and Linux swap
subsystem:

* filesystem - must be only single device
* filesystem - must have only *single* data profile
* subvolume - cannot be snapshotted if it contains any active swapfiles
* swapfile - must be preallocated (i.e. no holes)
* swapfile - must be NODATACOW (i.e. also NODATASUM, no compression)

The limitations come namely from the COW-based design and mapping layer of
blocks that allows the advanced features like relocation and multi-device
filesystems. However, the swap subsystem expects simpler mapping and no
background changes of the file block location once they've been assigned to
swap.

With active swapfiles, the following whole-filesystem operations will skip
swapfile extents or may fail:

* balance - block groups with extents of any active swapfiles are skipped and
  reported, the rest will be processed normally
* resize grow - unaffected
* resize shrink - works as long as the extents of any active swapfiles are
  outside of the shrunk range
* device add - if the new devices do not interfere with any already active swapfiles
  this operation will work, though no new swapfile can be activated
  afterwards
* device delete - if the device has been added as above, it can be also deleted
* device replace - ditto

When there are no active swapfiles and a whole-filesystem exclusive operation
is running (e.g. balance, device delete, shrink), the swapfiles cannot be
temporarily activated. The operation must finish first.

To create and activate a swapfile run the following commands:

.. code-block:: bash

        # truncate -s 0 swapfile
        # chattr +C swapfile
        # fallocate -l 2G swapfile
        # chmod 0600 swapfile
        # mkswap swapfile
        # swapon swapfile

Since version 6.1 it's possible to create the swapfile in a single command
(except the activation):

.. code-block:: bash

        # btrfs filesystem mkswapfile swapfile
        # swapon swapfile

Please note that the UUID returned by the *mkswap* utility identifies the swap
"filesystem" and because it's stored in a file, it's not generally visible and
usable as an identifier unlike if it was on a block device.

Once activated the file will appear in */proc/swaps*:

.. code-block:: none

        # cat /proc/swaps
        Filename          Type          Size           Used      Priority
        /path/swapfile    file          2097152        0         -2

The swapfile can be created as one-time operation or, once properly created,
activated on each boot by the **swapon -a** command (usually started by the
service manager). Add the following entry to */etc/fstab*, assuming the
filesystem that provides the */path* has been already mounted at this point.
Additional mount options relevant for the swapfile can be set too (like
priority, not the BTRFS mount options).

.. code-block:: none

        /path/swapfile        none        swap        defaults      0 0

From now on the subvolume with the active swapfile cannot be snapshotted until
the swapfile is deactivated again by ``swapoff``. Then the swapfile is a
regular file and the subvolume can be snapshotted again, though this would prevent
another activation any swapfile that has been snapshotted. New swapfiles (not
snapshotted) can be created and activated.

Otherwise, an inactive swapfile does not affect the containing subvolume. Activation
creates a temporary in-memory status and prevents some file operations, but is
not stored permanently.

Hibernation
-----------

A swapfile can be used for hibernation but it's not straightforward. Before
hibernation a resume offset must be written to file */sys/power/resume_offset*
or the kernel command line parameter *resume_offset* must be set.

The value is the physical offset on the device. Note that **this is not the same
value that** ``filefrag`` **prints as physical offset!**

Btrfs filesystem uses mapping between logical and physical addresses but here
the physical can still map to one or more device-specific physical block
addresses. It's the device-specific physical offset that is suitable as resume
offset.

Since version 6.1 there's a command ``btrfs inspect-internal map-swapfile`` that will
print the device physical offset and the adjusted value for */sys/power/resume_offset*.
Note that the value is divided by page size, i.e. it's not the offset itself.

.. code-block:: bash

        # btrfs filesystem mkswapfile swapfile
        # btrfs inspect-internal map-swapfile swapfile
        Physical start: 811511726080
        Resume offset:     198122980

For scripting and convenience the option *-r* will print just the offset:

.. code-block:: bash

        # btrfs inspect-internal map-swapfile -r swapfile
        198122980

The command *map-swapfile* also verifies all the requirements, i.e. no holes,
single device, etc.


Troubleshooting
---------------

If the swapfile activation fails please verify that you followed all the steps
above or check the system log (e.g. ``dmesg`` or ``journalctl``) for more
information.

Notably, the *swapon* utility exits with a message that does not say what
failed:

.. code-block:: none

        # swapon /path/swapfile
	swapon: /path/swapfile: swapon failed: Invalid argument

The specific reason is likely to be printed to the system log by the btrfs
module:

.. code-block:: none

	# journalctl -t kernel | grep swapfile
	kernel: BTRFS warning (device sda): swapfile must have single data profile
