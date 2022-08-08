The btrfs filesystem supports setting file attributes or flags. Note there are
old and new interfaces, with confusing names. The following list should clarify
that:

* *attributes*: ``chattr(1)`` or ``lsattr(1)`` utilities (the ioctls are
  FS_IOC_GETFLAGS and FS_IOC_SETFLAGS), due to the ioctl names the attributes
  are also called flags
* *xflags*: to distinguish from the previous, it's extended flags, with tunable
  bits similar to the attributes but extensible and new bits will be added in
  the future (the ioctls are FS_IOC_FSGETXATTR and FS_IOC_FSSETXATTR but they
  are not related to extended attributes that are also called xattrs), there's
  no standard tool to change the bits, there's support in ``xfs_io(8)`` as
  command **xfs_io -c chattr**

Attributes
^^^^^^^^^^

a
        *append only*, new writes are always written at the end of the file

A
        *no atime updates*

c
        *compress data*, all data written after this attribute is set will be compressed.
        Please note that compression is also affected by the mount options or the parent
        directory attributes.

        When set on a directory, all newly created files will inherit this attribute.
        This attribute cannot be set with 'm' at the same time.

C
        *no copy-on-write*, file data modifications are done in-place

        When set on a directory, all newly created files will inherit this attribute.

        .. note::
                Due to implementation limitations, this flag can be set/unset only on
                empty files.

d
        *no dump*, makes sense with 3rd party tools like ``dump(8)``, on BTRFS the
        attribute can be set/unset but no other special handling is done

D
        *synchronous directory updates*, for more details search ``open(2)`` for *O_SYNC*
        and *O_DSYNC*

i
        *immutable*, no file data and metadata changes allowed even to the root user as
        long as this attribute is set (obviously the exception is unsetting the attribute)

m
        *no compression*, permanently turn off compression on the given file. Any
        compression mount options will not affect this file. (``chattr`` support added in
        1.46.2)

        When set on a directory, all newly created files will inherit this attribute.
        This attribute cannot be set with *c* at the same time.

S
        *synchronous updates*, for more details search ``open(2)`` for *O_SYNC* and
        *O_DSYNC*

No other attributes are supported.  For the complete list please refer to the
``chattr(1)`` manual page.

XFLAGS
^^^^^^

There's overlap of letters assigned to the bits with the attributes, this list
refers to what ``xfs_io(8)`` provides:

i
        *immutable*, same as the attribute

a
        *append only*, same as the attribute

s
        *synchronous updates*, same as the attribute *S*

A
        *no atime updates*, same as the attribute

d
        *no dump*, same as the attribute

