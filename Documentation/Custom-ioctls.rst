Custom ioctls
=============

Filesystems are usually extended by custom ioctls beyond the standard system
call interface to let user applications access the advanced features. They're
low level and the following list gives only an overview of the capabilities or
a command if available:

- reverse lookup, from file offset to inode, as command :command:`btrfs inspect-internal logical-resolve`

- resolve inode number to list of names, as command :command:`btrfs inspect-internal inode-resolve`

- tree search, given a key range and tree id, lookup and return all b-tree items
  found in that range, basically all metadata at your hand but you need to know
  what to do with them, the ioctl is privileged as it has full access to all
  filesystem metadata

- informative, about devices, space allocation or the whole filesystem, many of
  which is also exported in :file:`/sys/fs/btrfs/`

- query/set a subset of features on a mounted filesystem

Programming documentation of the ioctls is in the manual page
:doc:`btrfs-ioctl`.
