Btrees
======

B-trees Introduction
--------------------

Btrfs uses a single set of btree manipulation code for all metadata in
the filesystem. For performance or organizational purposes, the trees
are broken up into a few different types, and each type of tree will
hold a few different types of keys. The super block holds pointers to
the tree roots of the tree of tree roots and the chunk tree.


Tree of Tree roots
------------------

This tree is used for indexing and finding the root of most of the other
trees in the filesystem. It attaches names to subvolumes and snapshots,
and stores the location of the extent allocation tree root. It also
stores pointers to all of the subvolumes or snapshots that are being
deleted by the transaction code. This allows the deletion to pick up
where it left off after a crash.


Chunk Tree
----------

The chunk tree does all of the logical to physical block address mapping
for the filesystem, and it stores information about all of the devices
in the FS. In order to bootstrap lookup in the chunk tree, the super
block also duplicates the chunk items needed to resolve blocks in the
chunk tree. Over time, the chunk tree will be split into multiple roots
to allow access of larger storage pools.

There are back references from the chunk items to the extent tree that
allocated them. Only a single extent tree can allocate extents out of a
given chunk.

Two types of key are stored in the chunk tree:

-  DEV_ITEM (where the offset field is the internal devid), which
   contain information on all of the underlying block devices in the
   filesystem
-  CHUNK_ITEM (where the offset field is the start of the chunk as a
   virtual address), which maps a section of the virtual address space
   (a chunk) into physical storage.


Device Allocation Tree
----------------------

The device allocation tree records which parts of each physical device
have been allocated into chunks. This is a relatively small tree that is
only updated as new chunks are allocated. It stores back references to
the chunk tree that allocated each physical extent on the device.


Extent Allocation Tree
----------------------

The extent allocation tree records byte ranges that are in use,
maintains reference counts on each extent and records back references to
the tree or file that is using each extent. Logical block groups are
created inside the extent allocation tree, and these reference large
logical extents from the chunk tree.

Each block group can only store a specific type of extent. This might
include metadata, or mirrored metadata, or striped data blocks etc.

Currently there is only one extent allocation tree shared by all the
other trees. This will change in order to scale better under load.

Keys for the extent tree use the start of the extent as the objectid. A BLOCK_GROUP_ITEM key will be followed by the EXTENT_ITEM keys for extents within that block group.


FS Trees
--------

These store files and directories, and all of the normal metadata you
would expect to find in a filesystem. There is one root for each
subvolume or snapshot, but snapshots will share blocks between roots.

Keys in FS trees always use the inode number of the filesystem object as the objectid.

Each object will have one or more of:

-  Inode.
-  Inode ref, indicating what name this object is known as, and in which
   directory.
-  For files, a set of extent information, indicating where on the
   filesystem this file's data is.
-  For directories, two sequences of dir_items, one indexed by a hash of
   the object name, and one indexed by a unique sequential index number.


Checksum Tree
-------------

The checksum tree stores block checksums. Every 4k block of data stored
on disk has a checksum associated with it. The "offset" part of the keys
in the checksum tree indicates the start of the checksummed data on
disk. The value stored with the key is a sequence of (currently 4-byte)
checksums, for the 4k blocks starting at the offset.


Data Relocation Tree
--------------------


Log Root Tree
-------------


UUID Tree
---------

The tree storesc correspondence between UUIDs and subvolumes. Used for
quick lookup during send.


Quota Tree
----------

The qgroup information about status and qgroup relations are stored in this tree.
The tree exists only when quotas are enabled.


Free Space Tree
---------------

This tree implements *space_cache=v2*, which is a tree-based tracking of free
space. Successor of the v1 code which used inodes to store the space information.


Block Group Tree
----------------

Separate tree (and feature) that stores only block group items and allows quick lookup
during mount. Otherwise the block group items are scattered in the Extent tree
and cause slow mount due to excessive seeking.


Raid Stripe Tree
----------------

A separate tracking of file extents and block groups that allows more flexible
location of physical offsets while keeping the logical offsets the same. This
is used by zoned mode and raid56.
