A BTRFS subvolume is a part of filesystem with its own independent
file/directory hierarchy and inode number namespace. Subvolumes can share file
extents. A snapshot is also subvolume, but with a given initial content of the
original subvolume. A subvolume has always inode number 256 (see more in
:docref:`Inode numbers <Subvolumes:subvolume-inode-numbers>`).

.. note::
   A subvolume in BTRFS is not like an LVM logical volume, which is block-level
   snapshot while BTRFS subvolumes are file extent-based.

A subvolume looks like a normal directory, with some additional operations
described below. Subvolumes can be renamed or moved, nesting subvolumes is not
restricted but has some implications regarding snapshotting. The numeric id
(called *subvolid* or *rootid*) of the subvolume is persistent and cannot be
changed.

A subvolume in BTRFS can be accessed in two ways:

- like any other directory that is accessible to the user
- like a separately mounted filesystem (options *subvol* or *subvolid*)

In the latter case the parent directory is not visible and accessible. This is
similar to a bind mount, and in fact the subvolume mount does exactly that.

A freshly created filesystem is also a subvolume, called *top-level*,
internally has an id 5. This subvolume cannot be removed or replaced by another
subvolume. This is also the subvolume that will be mounted by default, unless
the default subvolume has been changed (see :ref:`btrfs subvolume set-default<man-subvolume-set-default>`).

A snapshot is a subvolume like any other, with given initial content. By
default, snapshots are created read-write. File modifications in a snapshot
do not affect the files in the original subvolume.

Subvolumes can be given capacity limits, through the qgroups/quota facility, but
otherwise share the single storage pool of the whole BTRFS filesystem. They may
even share data between themselves (through deduplication or snapshotting).

.. note::
    A snapshot is not a backup: snapshots work by use of BTRFS' copy-on-write
    behaviour. A snapshot and the original it was taken from initially share all
    of the same data blocks. If that data is damaged in some way (cosmic rays,
    bad disk sector, accident with dd to the disk), then the snapshot and the
    original will both be damaged. Snapshots are useful to have local online
    "copies" of the filesystem that can be referred back to, or to implement a
    form of deduplication, or to fix the state of a filesystem for making a full
    backup without anything changing underneath it. They do not in themselves
    make your data any safer.

Subvolume flags
---------------

The subvolume flag currently implemented is the *ro* property (read-only
status). Read-write subvolumes have that set to *false*, snapshots as *true*.
In addition to that, a plain snapshot will also have last change generation and
creation generation equal.

Read-only snapshots are building blocks of incremental send (see
:doc:`btrfs-send`) and the whole use case relies on unmodified snapshots where
the relative changes are generated from. Thus, changing the subvolume flags
from read-only to read-write will break the assumptions and may lead to
unexpected changes in the resulting incremental stream.

A snapshot that was created by send/receive will be read-only, with different
last change generation, and with set *received_uuid* which identifies
the subvolume on the filesystem that produced the stream. The use case relies
on matching data on both sides. Changing the subvolume to read-write after it
has been received requires to reset the *received_uuid*. As this is a notable
change and could potentially break the incremental send use case, performing
it by :ref:`btrfs property set<man-property-set>` requires force if that is
really desired by user.

.. note::
   The safety checks have been implemented in 5.14.2, any subvolumes previously
   received (with a valid *received_uuid*) and read-write status may exist and
   could still lead to problems with send/receive. You can use :ref:`btrfs subvolume show<man-subvolume-show>`
   to identify them. Flipping the flags to read-only and back to
   read-write will reset the *received_uuid* manually.  There may exist a
   convenience tool in the future.

Nested subvolumes
-----------------

There are no restrictions for subvolume creation, so it's up to the user how to
organize them, whether to have a flat layout (all subvolumes are direct
descendants of the toplevel one), or nested.

What should be mentioned early is that a snapshotting is not recursive, so a
subvolume or a snapshot is effectively a barrier and no files in the nested subvolumes
appear in the snapshot. Instead, there's a stub subvolume, also sometimes called
*empty subvolume*, with the same name as original subvolume and with inode number 2.
This can be used intentionally but could be confusing in case of nested layouts.

.. code-block:: bash

   $ btrfs subvolume create subvol1
   $ btrfs subvolume create subvol1/subvol2
   $ btrfs subvolume snapshot subvol1 snap1
   $ find -ls
   121093  0  drwxr-xr-x  1  user  users    24  Jul 30  12:34  .
      256  0  drwxr-xr-x  1  user  users    14  Jul 30  12:34  ./subvol1
      256  0  drwxr-xr-x  1  user  users     0  Jul 30  12:34  ./subvol1/subvol2
      257  0  -rw-r--r--  1  user  users     0  Jul 30  12:34  ./subvol1/subvol2/file
      256  0  drwxr-xr-x  1  user  users    14  Jul 30  12:34  ./snap1
        2  0  drwxr-xr-x  1  user  users     0  Jul 30  12:34  ./snap1/subvol2

The numbers in the first columns are inode numbers, 256 is for a regular
subvolume (or snapshot), 2 is the *empty subvolume*. The snapshotted directory
representing *subvol2* does not contain the *file*.

.. note::
   The *empty subvolume* will not be sent (:doc:`btrfs-send`) and thus will not be created on
   the receive side (:doc:`btrfs-receive`).

Case study: system root layouts
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

There are two ways how the system root directory and subvolume layout could be
organized. The interesting use case for root is to allow rollbacks to previous
version, as one atomic step. If the entire filesystem hierarchy starting in :file:`/`
is in one subvolume, taking snapshot will encompass all files. This is easy for
the snapshotting part but has undesirable consequences for rollback. For example,
log files would get rolled back too, or any data that are stored on the root
filesystem but are not meant to be rolled back either (database files, VM
images, ...).

Here we could utilize the snapshotting barrier mentioned above, making each directory
that stores data to be preserved across rollbacks its own subvolume. This
could be e.g. :file:`/var`. Further more fine-grained partitioning could be done, e.g.
adding separate subvolumes for :file:`/var/log`, :file:`/var/cache` etc.

The fact that there are separate subvolumes requires separate actions to take the
snapshots (here, it gets disconnected from the system root snapshots). This needs
to be taken care of by system tools, installers, together with selection of which
directories are highly recommended to be separate subvolumes.

Mount options
-------------

Mount options are of two kinds, generic (that are handled by VFS layer) and
specific, handled by the filesystem. The following list shows which are
applicable to individual subvolume mounts, while there are more options that
always affect the whole filesystem:

- Generic: noatime/relatime/..., nodev, nosuid, ro, rw, dirsync
- Filesystem-specific: compress, autodefrag, nodatacow, nodatasum

Examples of whole filesystem options are e.g. *space_cache*, *rescue*, *device*,
*skip_balance*, etc. The exceptional options are *subvol* and *subvolid* that
are actually used for mounting a given subvolume and can be specified only once
for the mount.

Subvolumes belong to a single filesystem and, as implemented now, all share the
same specific mount options. Also, changes done by remount have immediate effect. This
may change in the future.

Mounting a read-write snapshot as read-only is possible and will not change the
*ro* property and flag of the subvolume.

The name of the mounted subvolume is stored in file :file:`/proc/self/mountinfo` in
the 4th column:

.. code-block:: none

   27 21 0:19 /subv1 /mnt rw,relatime - btrfs /dev/sda rw,space_cache
              ^^^^^^

.. duplabel:: subvolume-inode-numbers

Inode numbers
-------------

A directory representing a subvolume has always inode number 256 (sometimes
also called a root of the subvolume):

.. code-block:: none

   $ ls -lis
   total 0
   389111 0 drwxr-xr-x 1 user users 0 Jan 20 12:13 dir
   389110 0 -rw-r--r-- 1 user users 0 Jan 20 12:13 file
      256 0 drwxr-xr-x 1 user users 0 Jan 20 12:13 snap1
      256 0 drwxr-xr-x 1 user users 0 Jan 20 12:13 subv1

If a subvolume is nested and then a snapshot is taken, then the cloned
directory entry representing the subvolume becomes empty and the inode has
number 2. All other files and directories in the target snapshot preserve their
original inode numbers.

.. note::
   Inode number is not a filesystem-wide unique identifier, some applications
   assume that. Please use the *subvolumeid:inodenumber* pair for that purpose.
   The subvolume id can be read by :ref:`btrfs inspect-internal rootid<man-inspect-rootid>`
   or by the ioctl :ref:`BTRFS_IOC_INO_LOOKUP`.

Performance
-----------

Subvolume creation needs to flush dirty data that belong to the subvolume and this
step may take some time. Otherwise, once there's nothing else to do, the snapshot
is instantaneous and only creates a new tree root copy in the metadata.

Snapshot deletion has two phases: first its directory is deleted and the
subvolume is added to a queuing list, then the list is processed one by one and the
data related to the subvolume get deleted. This is usually called *cleaning* and
can take some time depending on the amount of shared blocks (can be a lot of
metadata updates), and the number of currently queued deleted subvolumes.
