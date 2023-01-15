btrfs-ioctl(3)
==============

NAME
----

btrfs-ioctl - documentation for the ioctl interface to btrfs

DESCRIPTION
-----------

The ioctl() system call is a way how to request custom actions performed on a
filesystem beyond the standard interfaces (like syscalls).  An ioctl is
specified by a number and an associated data structure that implement a
feature, usually not available in other filesystems. The number of ioctls grows
over time and in some cases get promoted to a VFS-level ioctl once other
filesystems adopt the functionality. Backward compatibility is maintained
and a formerly private ioctl number could become available on the VFS level.


DATA STRUCTURES AND DEFINITIONS
-------------------------------

.. code-block::

   struct btrfs_ioctl_vol_args {
           __s64 fd;
           char name[BTRFS_PATH_NAME_MAX + 1];
   };

.. code-block::

   struct btrfs_ioctl_vol_args_v2 {
           __s64 fd;
           __u64 transid;
           __u64 flags;
           union {
                   struct {
                           __u64 size;
                           struct btrfs_qgroup_inherit __user *qgroup_inherit;
                   };
                   __u64 unused[4];
           };
           union {
               char name[BTRFS_SUBVOL_NAME_MAX + 1];
               __u64 devid;
               __u64 subvolid;
            };
   };

.. code-block::

   struct btrfs_ioctl_get_subvol_info_args {
        /* Id of this subvolume */
        __u64 treeid;

        /* Name of this subvolume, used to get the real name at mount point */
        char name[BTRFS_VOL_NAME_MAX + 1];

        /*
         * Id of the subvolume which contains this subvolume.
         * Zero for top-level subvolume or a deleted subvolume.
         */
        __u64 parent_id;

        /*
         * Inode number of the directory which contains this subvolume.
         * Zero for top-level subvolume or a deleted subvolume
         */
        __u64 dirid;

        /* Latest transaction id of this subvolume */
        __u64 generation;

        /* Flags of this subvolume */
        __u64 flags;

        /* UUID of this subvolume */
        __u8 uuid[BTRFS_UUID_SIZE];

        /*
         * UUID of the subvolume of which this subvolume is a snapshot.
         * All zero for a non-snapshot subvolume.
         */
        __u8 parent_uuid[BTRFS_UUID_SIZE];

        /*
         * UUID of the subvolume from which this subvolume was received.
         * All zero for non-received subvolume.
         */
        __u8 received_uuid[BTRFS_UUID_SIZE];

        /* Transaction id indicating when change/create/send/receive happened */
        __u64 ctransid;
        __u64 otransid;
        __u64 stransid;
        __u64 rtransid;
        /* Time corresponding to c/o/s/rtransid */
        struct btrfs_ioctl_timespec ctime;
        struct btrfs_ioctl_timespec otime;
        struct btrfs_ioctl_timespec stime;
        struct btrfs_ioctl_timespec rtime;

        /* Must be zero */
        __u64 reserved[8];
   };

.. code-block::

   BTRFS_SUBVOL_NAME_MAX = 4039
   BTRFS_PATH_NAME_MAX = 4087

OVERVIEW
--------

The ioctls are defined by a number and associated with a data structure that
contains further information. All ioctls use file descriptor (fd) as a reference
point, it could be the filesystem or a directory inside the filesystem.

An ioctl can be used in the following schematic way:

.. code-block::

   struct btrfs_ioctl_args args;

   memset(&args, 0, sizeof(args));
   args.key = value;
   ret = ioctl(fd, BTRFS_IOC_NUMBER, &args);

The 'fd' is the entry point to the filesystem and for most ioctls it does not
matter which file or directory is that. Where it matters it's explicitly
mentioned. The 'args' is the associated data structure for the request. It's
strongly recommended to initialize the whole structure to zeros as this is
future-proof when the ioctl gets further extensions. Not doing that could lead
to mismatch of old userspace and new kernel versions, or vice versa.
The 'BTRFS_IOC_NUMBER' is says which operation should be done on the given
arguments. Some ioctls take a specific data structure, some of them share a
common one, no argument structure ioctls exist too.

The library 'libbtrfsutil' wraps a few ioctls for convenience. Using raw ioctls
is not discouraged but may be cumbersome though it does not need additional
library dependency. Backward compatibility is guaranteed and incompatible
changes usually lead to a new version of the ioctl. Enhancements of existing
ioctls can happen and depend on additional flags to be set. Zeroed unused
space is commonly understood as a mechanism to communicate the compatibility
between kernel and userspace and thus zeroing is really important. In exceptional
cases this is not enough and further flags need to be passed to distinguish
between zero as implicit unused initialization and a valid zero value. Such
cases are documented.

LIST OF IOCTLS
--------------

* BTRFS_IOC_SUBVOL_CREATE -- (obsolete) create a subvolume
* BTRFS_IOC_SNAP_CREATE
* BTRFS_IOC_DEFRAG
* BTRFS_IOC_RESIZE
* BTRFS_IOC_SCAN_DEV
* BTRFS_IOC_SYNC
* BTRFS_IOC_CLONE
* BTRFS_IOC_ADD_DEV
* BTRFS_IOC_RM_DEV
* BTRFS_IOC_BALANCE
* BTRFS_IOC_CLONE_RANGE
* BTRFS_IOC_SUBVOL_CREATE
* BTRFS_IOC_SNAP_DESTROY
* BTRFS_IOC_DEFRAG_RANGE
* BTRFS_IOC_TREE_SEARCH
* BTRFS_IOC_TREE_SEARCH_V2
* BTRFS_IOC_INO_LOOKUP
* BTRFS_IOC_DEFAULT_SUBVOL
* BTRFS_IOC_SPACE_INFO
* BTRFS_IOC_START_SYNC
* BTRFS_IOC_WAIT_SYNC
* BTRFS_IOC_SNAP_CREATE_V2 -- create a snapshot of a subvolume
* BTRFS_IOC_SUBVOL_CREATE_V2 -- create a subvolume
* BTRFS_IOC_SUBVOL_GETFLAGS -- get flags of a subvolume
* BTRFS_IOC_SUBVOL_SETFLAGS -- set flags of a subvolume
* BTRFS_IOC_SCRUB
* BTRFS_IOC_SCRUB_CANCEL
* BTRFS_IOC_SCRUB_PROGRESS
* BTRFS_IOC_DEV_INFO
* BTRFS_IOC_FS_INFO
* BTRFS_IOC_BALANCE_V2
* BTRFS_IOC_BALANCE_CTL
* BTRFS_IOC_BALANCE_PROGRESS
* BTRFS_IOC_INO_PATHS
* BTRFS_IOC_LOGICAL_INO
* BTRFS_IOC_SET_RECEIVED_SUBVOL
* BTRFS_IOC_SEND
* BTRFS_IOC_DEVICES_READY
* BTRFS_IOC_QUOTA_CTL
* BTRFS_IOC_QGROUP_ASSIGN
* BTRFS_IOC_QGROUP_CREATE
* BTRFS_IOC_QGROUP_LIMIT
* BTRFS_IOC_QUOTA_RESCAN
* BTRFS_IOC_QUOTA_RESCAN_STATUS
* BTRFS_IOC_QUOTA_RESCAN_WAIT
* BTRFS_IOC_GET_FSLABEL
* BTRFS_IOC_SET_FSLABEL
* BTRFS_IOC_GET_DEV_STATS
* BTRFS_IOC_DEV_REPLACE
* BTRFS_IOC_FILE_EXTENT_SAME
* BTRFS_IOC_GET_FEATURES
* BTRFS_IOC_SET_FEATURES
* BTRFS_IOC_GET_SUPPORTED_FEATURES
* BTRFS_IOC_RM_DEV_V2
* BTRFS_IOC_LOGICAL_INO_V2
* BTRFS_IOC_GET_SUBVOL_INFO -- get information about a subvolume
* BTRFS_IOC_GET_SUBVOL_ROOTREF
* BTRFS_IOC_INO_LOOKUP_USER
* BTRFS_IOC_SNAP_DESTROY_V2 -- destroy a (snapshot or regular) subvolume

DETAILED DESCRIPTION
--------------------

BTRFS_IOC_SUBVOL_CREATE
~~~~~~~~~~~~~~~~~~~~~~~

.. note::
   obsoleted by BTRFS_IOC_SUBVOL_CREATE_V2

*(since: 3.0, obsoleted: 4.0)* Create a subvolume.

ioctl fd
    file descriptor of the parent directory of the new subvolume
argument type
    struct btrfs_ioctl_vol_args
fd
    ignored
name
    name of the subvolume, although the buffer can be almost 4k, the file
    size is limited by Linux VFS to 255 characters and must not contain a slash
    ('/')

BTRFS_IOC_SNAP_CREATE_V2
~~~~~~~~~~~~~~~~~~~~~~~~

.. note::
   obsoletes BTRFS_IOC_SNAP_CREATE

Create a snapshot of a subvolume.

ioctl fd
    file descriptor of the directory inside which to create the new snapshot
argument type
    struct btrfs_ioctl_vol_args_v2
fd
    file descriptor of any directory inside the subvolume to snapshot
transid
    ignored
flags
    any subset of `BTRFS_SUBVOL_RDONLY` to make the new snapshot read-only, or
    `BTRFS_SUBVOL_QGROUP_INHERIT` to apply the `qgroup_inherit` field
name
    the name, under the ioctl fd, for the new subvolume

BTRFS_IOC_SUBVOL_CREATE_V2
~~~~~~~~~~~~~~~~~~~~~~~~~~

.. note::
   obsoletes BTRFS_IOC_SUBVOL_CREATE

*(since: 3.6)* Create a subvolume, qgroup inheritance can be specified.

ioctl fd
    file descriptor of the parent directory of the new subvolume
argument type
    struct btrfs_ioctl_vol_args_v2
fd
    ignored
transid
    ignored
flags
    ignored
size
    ...
qgroup_inherit
    ...
name
    name of the subvolume, although the buffer can be almost 4k, the file size
    is limited by Linux VFS to 255 characters and must not contain a slash ('/')
devid
    ...

BTRFS_IOC_SUBVOL_GETFLAGS
~~~~~~~~~~~~~~~~~~~~~~~~~

Read the flags of a subvolume. The returned flags are either 0 or
`BTRFS_SUBVOL_RDONLY`.

ioctl fd
    file descriptor of the subvolume to examine
argument type
    uint64_t

BTRFS_IOC_SUBVOL_SETFLAGS
~~~~~~~~~~~~~~~~~~~~~~~~~

Change the flags of a subvolume.

ioctl fd
    file descriptor of the subvolume to modify
argument type
    uint64_t, either 0 or `BTRFS_SUBVOL_RDONLY`

BTRFS_IOC_GET_SUBVOL_INFO
~~~~~~~~~~~~~~~~~~~~~~~~~

Get information about a subvolume.

ioctl fd
    file descriptor of the subvolume to examine
argument type
    struct btrfs_ioctl_get_subvol_info_args

BTRFS_IOC_SNAP_DESTROY_V2
~~~~~~~~~~~~~~~~~~~~~~~~~

Destroy a subvolume, which may or may not be a snapshot.

ioctl fd
    if `flags` does not include `BTRFS_SUBVOL_SPEC_BY_ID`, or if executing in a
    non-root user namespace, file descriptor of the parent directory containing
    the subvolume to delete; otherwise, file descriptor of any directory on the
    same filesystem as the subvolume to delete, but not within the same
    subvolume
argument type
    struct btrfs_ioctl_vol_args_v2
fd
    ignored
transid
    ignored
flags
    0 if the `name` field identifies the subvolume by name in the specified
    directory, or `BTRFS_SUBVOL_SPEC_BY_ID` if the `subvolid` field specifies
    the ID of the subvolume
name
    only if `flags` does not contain `BTRFS_SUBVOL_SPEC_BY_ID`, the name
    (within the directory identified by `fd`) of the subvolume to delete
subvolid
    only if `flags` contains `BTRFS_SUBVOL_SPEC_BY_ID`, the subvolume ID of the
    subvolume to delete

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.  Please refer to the documentation at
https://btrfs.readthedocs.io or wiki http://btrfs.wiki.kernel.org for further
information.

SEE ALSO
--------
ioctl(2)
