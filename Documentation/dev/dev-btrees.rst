Btrees
======

B-trees introduction
--------------------

Btrfs uses a single set of btree manipulation code for all metadata in the
filesystem. For performance or organizational purposes, the trees are broken up
into a few different types, and each type of tree holds different types of
keys. The super block holds pointers to the tree roots of the tree of tree
roots and the chunk tree.


Tree of tree roots (tree_root)
------------------------------

This tree stores pointers to other trees in the filesystem. It holds the
association of subvolume names and their tree, pointer to the default subvolume
and tracks deletion progress of a subvolume.


Chunk Tree
----------

The chunk tree does all of the logical to physical block address mapping
for the filesystem, and it stores information about all of the devices
in the FS. In order to bootstrap lookup in the chunk tree, the super
block also duplicates the chunk items needed to resolve blocks in the
chunk tree.

There are back references from the chunk items to the extent tree that
allocated them. Only a single extent tree can allocate extents out of a
given chunk.

Two types of key are stored in the chunk tree:

-  DEV_ITEM (where the offset field is the devid), which contain information on
   all of the underlying block devices in the filesystem
-  CHUNK_ITEM (where the offset field is the start of the chunk as a
   virtual address), which maps a section of the virtual address space
   (a chunk) into physical storage.


Device Allocation Tree
----------------------

The device allocation tree records which parts of each physical device
have been allocated into chunks. This is a relatively small tree that is
only updated as new chunks are allocated. It stores back references to
the chunk tree that allocated each physical extent on the device.


Extent Tree
-----------

The extent tree records byte ranges that are in use, maintains reference counts
on each extent and records back references to the tree or file that is using
each extent. Logical block groups are created inside the extent tree, and these
reference large logical extents from the chunk tree.

Each block group can only store a specific type of extent. This might include
metadata, or mirrored metadata, or striped data blocks etc.

Currently there is only one extent tree shared by all the other trees.

Keys for the extent tree use the start of the extent as the objectid. A
BLOCK_GROUP_ITEM key will be followed by the EXTENT_ITEM keys for extents within
that block group.


Filesystem tree(s)
------------------

The filesystem trees store files and directories of subvolumes.  There is one
root for each subvolume or snapshot. Extents can be shared among various
filesystem trees. The toplevel subvolume has a special filesystem tree that
always exists.

Keys in FS trees always use the inode number of the filesystem object as the
objectid.

Each object haves one or more of:

-  Inode.
-  Inode ref, indicating what name this object is known as, and in which
   directory.
-  For files, a set of extent information, indicating where on the
   filesystem this file's data is.
-  For directories, two sequences of dir_items, one indexed by a hash of
   the object name, and one indexed by a unique sequential index number.


Checksum tree
-------------

The checksum tree stores block checksums. Every 4k block of data stored
on disk has a checksum associated with it. The "offset" part of the keys
in the checksum tree indicates the start of the checksummed data on
disk. The value stored with the key is a sequence of checksums, for the 4k
blocks starting at the offset.


Data relocation tree
--------------------


Log root tree
-------------


UUID tree
---------

The tree stores correspondence between UUIDs and subvolumes. Used for
quick lookup during send.


Quota tree
----------

The qgroup information about status and qgroup relations are stored in this tree.
The tree exists only when quotas are enabled.


Free space tree
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
