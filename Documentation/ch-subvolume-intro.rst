A BTRFS subvolume is a part of filesystem with its own independent
file/directory hierarchy. Subvolumes can share file extents. A snapshot is also
subvolume, but with a given initial content of the original subvolume.

.. note::
   A subvolume in BTRFS is not like an LVM logical volume, which is block-level
   snapshot while BTRFS subvolumes are file extent-based.

A subvolume looks like a normal directory, with some additional operations
described below. Subvolumes can be renamed or moved, nesting subvolumes is not
restricted but has some implications regarding snapshotting.

A subvolume in BTRFS can be accessed in two ways:

* like any other directory that is accessible to the user
* like a separately mounted filesystem (options *subvol* or *subvolid*)

In the latter case the parent directory is not visible and accessible. This is
similar to a bind mount, and in fact the subvolume mount does exactly that.

A freshly created filesystem is also a subvolume, called *top-level*,
internally has an id 5. This subvolume cannot be removed or replaced by another
subvolume. This is also the subvolume that will be mounted by default, unless
the default subvolume has been changed (see ``btrfs subvolume set-default``).

A snapshot is a subvolume like any other, with given initial content. By
default, snapshots are created read-write. File modifications in a snapshot
do not affect the files in the original subvolume.

Subvolume flags
---------------

The subvolume flag currently implemented is the *ro* property. Read-write
subvolumes have that set to *false*, snapshots as *true*. In addition to that,
a plain snapshot will also have last change generation and creation generation
equal.

Read-only snapshots are building blocks of incremental send (see
``btrfs-send(8)``) and the whole use case relies on unmodified snapshots where
the relative changes are generated from. Thus, changing the subvolume flags
from read-only to read-write will break the assumptions and may lead to
unexpected changes in the resulting incremental stream.

A snapshot that was created by send/receive will be read-only, with different
last change generation, read-only and with set *received_uuid* which identifies
the subvolume on the filesystem that produced the stream. The use case relies
on matching data on both sides. Changing the subvolume to read-write after it
has been received requires to reset the *received_uuid*. As this is a notable
change and could potentially break the incremental send use case, performing
it by **btrfs property set** requires force if that is really desired by user.

.. note::
   The safety checks have been implemented in 5.14.2, any subvolumes previously
   received (with a valid *received_uuid*) and read-write status may exist and
   could still lead to problems with send/receive. You can use **btrfs subvolume
   show** to identify them. Flipping the flags to read-only and back to
   read-write will reset the *received_uuid* manually.  There may exist a
   convenience tool in the future.
