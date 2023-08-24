.. BTRFS integration related pages index

Interoperability
================

.. _interop-cgroups:

cgroups
-------

Cgroups are supported for the IO controller, for compressed and uncompressed
data. This can be used to limit bandwidth or for accounting. The cgroups can
be configured directly or e.g. via systemd directives *IOAccounting*,
*IOWeight* etc.

See also:
https://www.man7.org/linux/man-pages/man5/systemd.resource-control.5.html

.. _interop-fsverity:

fsverity
--------

The fs-verity is a support layer that filesystems can hook into to
support transparent integrity and authenticity protection of read-only
files. This requires a separate management utility :command:`fsverity`.

See also:
https://www.kernel.org/doc/html/latest/filesystems/fsverity.html

.. _interop-idmapped:

idmapped mounts
---------------

Btrfs supports mount with UID/GID mapped according to another namespace since
version 5.15.

See also:
https://lwn.net/Articles/837566/

Device mapper, MD-RAID
----------------------

Btrfs works on top of device mapper (DM) and linux multi-device software RAID
(MD-RAID) block devices transparently without any need for additional
configuration. There is no integration so device failures are not handled
automatically in any way, must be resolved either in btrfs or on the DM/MD
layer.

The functionality of DM/MD may duplicate the one provided by btrfs (like
mirroring), it's possible to use it that way but is probably wasteful and may
degrade performance. Creating a filesystem on top of the multiplexed device is
likely the desired way.

overlayfs
---------

Since kernel 4.15 the btrfs filesystem can be used as *lower* filesystem
for overlayfs (supporting the rename modes of *exchange* and *whiteout*).

SELinux
-------

.. _interop-io-uring:

io_uring
--------

.. _interop-nfs:

NFS
---

NFS is supported. When exporting a subvolume it is recommended to use the
*fsid* option with a unique id in case the server needs to restart. This
is recommended namely when clients use the mount option *hard*.

Example of server side export:

.. code-block:: none

   /mnt/data/subvolume1      192.168.1.2/24(fsid=12345,rw,sync)
   /mnt/data/subvolume2      192.168.1.2/24(fsid=23456,rw,sync)

See also:
https://www.man7.org/linux/man-pages/man5/exports.5.html

.. _interop-samba:

Samba
-----

The Samba VFS module *btrfs* adds support for compression, snapshots and server-side
copy (backed by reflink/clone range ioctl).

See also:
https://wiki.samba.org/index.php/Server-Side_Copy#Btrfs_Enhanced_Server-Side_Copy_Offload
