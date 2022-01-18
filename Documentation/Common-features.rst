Common Linux features
=====================

The Linux operating system implements a POSIX standard interfaces and API with
additional interfaces. Many of them have become common in other filesystems. The
ones listed below have been added relatively recently and are considered
interesting for users:

birth/origin inode time
        a timestamp associated with an inode of when it was created, cannot be
        changed and requires the *statx* syscall to be read

statx
        an extended version of the *stat* syscall that provides extensible
        interface to read more information that are not available in original
        *stat*

fallocate modes
        the *fallocate* syscall allows to manipulate file extents like punching
        holes, preallocation or zeroing a range

FIEMAP
        an ioctl that enumerates file extents, related tool is ``filefrag``

filesystem label
        another filesystem identification, could be used for mount or for better
        recognition, can be set or read by an ioctl or by command ``btrfs
        filesystem label``

O_TMPFILE
        mode of open() syscall that creates a file with no associated directory
        entry, which makes it impossible to be seen by other processes and is
        thus safe to be used as a temporary file
        (https://lwn.net/Articles/619146/)

xattr, acl
        extended attributes (xattr) is a list of *key=value* pairs associated
        with a file, usually storing additional metadata related to security,
        access control list in particular (ACL) or properties (``btrfs
        property``)

cross-rename
        mode of *renameat2* syscall that can atomically swap 2 directory
        entries (files/directories/subvolumes)


File attributes, XFLAGS
-----------------------

.. include:: ch-file-attributes.rst
