On-disk Format
==============

This document describes the Btrfs on‐disk format.

.. note::

   This document contains outdated and incomplete information and has been
   copied from the original btrfs.wiki.kernel.org with little review.

Overview
~~~~~~~~

Aside from the superblock, Btrfs consists entirely of several trees. The trees
use copy-on-write.  Trees are stored in nodes, each of with belong to a level
in the b-tree structure. Internal nodes contain references to other internal
nodes on the next level, or to leaf nodes then the level reaches zero. Leaf
nodes contain various types of data structures, depending on the tree.

Btrfs makes a distinction between logical and physical addresses. Logical
addresses are used in the filesystem structures, while physical addresses are
simply byte offsets on a disk. One logical address may correspond to physical
addresses on any number of disks, depending on RAID settings. The chunk tree is
used to convert from logical addresses to physical addresses; the dev tree can
be used for the reverse.

For bootstrapping purposes, the superblock contains a subset of the chunk tree,
specifically it contains "chunk items" for all system chunks. The superblock
also contains a logical reference to root nodes in the root and chunk trees,
which can then be used to locate all the other trees and data stored.

TODO Subvolumes and snapshots.


Basic Structures
~~~~~~~~~~~~~~~~

Note that the fields are unsigned, so object ID −1 will be treated as
0xffffffffffffffff and sorted to the end of the tree. Since Btrfs uses
little‐endian, a simple byte‐by‐byte comparison of KEYs will not work.


   ====  ==== ==== ===================================================
   Off   Size Type Description
   ====  ==== ==== ===================================================
   0x0   0x8  UINT Object ID. Each tree has its own set of Object IDs.
   0x8   0x1  UINT `Item type <#Item_Types>`__.
   0x9   0x8  UINT Offset. The meaning depends on the item type.
   0x11
   ====  ==== ==== ===================================================

Btrfs uses `Unix time <http://en.wikipedia.org/wiki/Unix_time>`__.


   === ==== ==== ========================================================
   Off Size Type Description
   === ==== ==== ========================================================
   0x0 0x8  SINT Number of seconds since 1970-01-01T00:00:00Z.
   0x8 0x4  UINT Number of nanoseconds since the beginning of the second.
   0xc
   === ==== ==== ========================================================

Superblock
^^^^^^^^^^

The primary superblock is located at 0x10000 (64KiB). Mirror copies of the
superblock are located at physical addresses 0x4000000 (64 MiB) and
0x4000000000 (256GiB), if these locations are valid. Superblock copies are
updated simultaneously.  During mount btrfs' kernel module reads only the first
super block (at 64KiB), if an error is detected mounting fails.

Note that btrfs only recognizes disks with a valid 0x1 0000 superblock;
otherwise, there would be confusion with other filesystems.

TODO


   +--------+-------+-------+-------------------------------------------------------------------------+
   | Off    | Size  | Type  | Description                                                             |
   +========+=======+=======+=========================================================================+
   | 0x0    | 0x20  | CSUM  | Checksum of everything past this field (from 20 to 1000)                |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x20   | 0x10  | UUID  | FS UUID                                                                 |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x30   | 0x8   | UINT  | physical address of this block (different for mirrors)                  |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x38   | 0x8   |       | flags                                                                   |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x40   | 0x8   | ASCII | magic ("_BHRfS_M")                                                      |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x48   | 0x8   |       | generation                                                              |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x50   | 0x8   |       | logical address of the root tree root                                   |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x58   | 0x8   |       | logical address of the `chunk tree <#Chunk_tree_.283.29>`__ root        |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x60   | 0x8   |       | logical address of the log tree root                                    |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x68   | 0x8   |       | log_root_transid                                                        |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x70   | 0x8   |       | total_bytes                                                             |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x78   | 0x8   |       | bytes_used                                                              |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x80   | 0x8   |       | root_dir_objectid (usually 6)                                           |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x88   | 0x8   |       | num_devices                                                             |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x90   | 0x4   |       | sectorsize                                                              |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x94   | 0x4   |       | nodesize                                                                |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x98   | 0x4   |       | leafsize                                                                |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x9c   | 0x4   |       | stripesize                                                              |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0xa0   | 0x4   |       | sys_chunk_array_size                                                    |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0xa4   | 0x8   |       | chunk_root_generation                                                   |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0xac   | 0x8   |       | compat_flags                                                            |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0xb4   | 0x8   |       | compat_ro_flags - only implementations that support the flags can write |
   |        |       |       | to the filesystem                                                       |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0xbc   | 0x8   |       | incompat_flags - only implementations that support the flags can use    |
   |        |       |       | the filesystem                                                          |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0xc4   | 0x2   |       | csum_type - Btrfs currently uses the CRC32c little-endian hash function |
   |        |       |       | with seed -1.                                                           |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0xc6   | 0x1   |       | root_level                                                              |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0xc7   | 0x1   |       | chunk_root_level                                                        |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0xc8   | 0x1   |       | log_root_level                                                          |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0xc9   | 0x62  |       | `DEV_ITEM <#DEV_ITEM_.28d8.29>`__ data for this device                  |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x12b  | 0x100 |       | label (may not contain '/' or '\\\\')                                   |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x22b  | 0x8   |       | cache_generation                                                        |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x233  | 0x8   |       | uuid_tree_generation                                                    |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x23b  | 0xf0  |       | reserved /\* future expansion \*/                                       |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x2b   | 0x800 |       | sys_chunk_array:(*n* bytes valid) Contains (KEY,                        |
   |        |       |       | :ref:`CHUNK_ITEM<CHUNK_ITEM_CONTENT>`) pairs for all SYSTEM chunks.     |
   |        |       |       | This is needed to bootstrap the mapping from logical addresses to       |
   |        |       |       | physical.                                                               |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0xb2b  | 0x2a0 |       | Contain super_roots (4 btrfs_root_backup)                               |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0xdcb  | 0x235 |       | current unused                                                          |
   +--------+-------+-------+-------------------------------------------------------------------------+
   | 0x1000 |       |       |                                                                         |
   +--------+-------+-------+-------------------------------------------------------------------------+

Header
^^^^^^

This is the data stored at the start of every node. The data following it
depends on whether it is an internal or leaf node, both of which are described
below.


   +-------+------+-------+--------------------------------------------------------------------------+
   | Off   | Size | Type  | Description                                                              |
   +=======+======+=======+==========================================================================+
   | 0x0   | 0x20 | CSUM  | Checksum of everything after this field (from 20 to the end of the node) |
   +-------+------+-------+--------------------------------------------------------------------------+
   | 0x20  | 0x10 | UUID  | FS UUID                                                                  |
   +-------+------+-------+--------------------------------------------------------------------------+
   | 0x30  | 0x8  | UINT  | Logical address of this node                                             |
   +-------+------+-------+--------------------------------------------------------------------------+
   | 0x38  | 0x7  | FIELD | Flags                                                                    |
   +-------+------+-------+--------------------------------------------------------------------------+
   | 0x3f  | 0x1  | UINT  | Backref. Rev.: always 1 (MIXED) for new filesystems; 0 (OLD) indicates   |
   |       |      |       | an old filesystem.                                                       |
   +-------+------+-------+--------------------------------------------------------------------------+
   | 0x40  | 0x10 | UUID  | Chunk tree UUID                                                          |
   +-------+------+-------+--------------------------------------------------------------------------+
   | 0x50  | 0x8  | UINT  | Generation                                                               |
   +-------+------+-------+--------------------------------------------------------------------------+
   | 0x58  | 0x8  | UINT  | The ID of the tree that contains this node                               |
   +-------+------+-------+--------------------------------------------------------------------------+
   | 0x60  | 0x4  | UINT  | Number of items                                                          |
   +-------+------+-------+--------------------------------------------------------------------------+
   | 0x64  | 0x1  | UINT  | Level (0 for leaf nodes)                                                 |
   +-------+------+-------+--------------------------------------------------------------------------+
   | 0x65  |      |       |                                                                          |
   +-------+------+-------+--------------------------------------------------------------------------+


Internal Node
^^^^^^^^^^^^^

In internal nodes, the node header is followed by a number of key pointers.


   ===== ==== ==== ============
   Off   Size Type Description
   ===== ==== ==== ============
   0x0   0x11 KEY  key
   0x11  0x8  UINT block number
   0x19  0x8  UINT generation
   0x21
   ===== ==== ==== ============


   ====== ======= ======= ======= === ==========
   header key ptr key ptr key ptr ... free space
   ====== ======= ======= ======= === ==========


Leaf Node
^^^^^^^^^

In leaf nodes, the node header is followed by a number of items. The items'
data is stored at the end of the node, and the contents of the item data
depends on the item type stored in the key.


   ===== ==== ==== ==========================================
   Off   Size Type Description
   ===== ==== ==== ==========================================
   0x0   0x11 KEY  key
   0x11  0x4  UINT data offset relative to end of header (65)
   0x15  0x4  UINT data size
   0x19
   ===== ==== ==== ==========================================


   ====== ====== ====== === ====== ========== ====== === ====== ======
   header item 0 item 1 ... item N free space data N ... data 1 data 0
   ====== ====== ====== === ====== ========== ====== === ====== ======


Object Types
~~~~~~~~~~~~

TODO

Objects
~~~~~~~

ROOT_TREE (1)

The root tree holds ROOT_ITEMs, ROOT_REFs, and ROOT_BACKREFs for every tree other than itself. It is
used to find the other trees and to determine the subvolume structure. It also holds the items for
the `root tree directory <#Root_tree_directory>`__. The logical address of the root tree is stored
in the `superblock <#Superblock>`__.


Reserved objectids
^^^^^^^^^^^^^^^^^^

There are several well-known objectids that refer to internal trees.

All root objectids between
``BTRFS_FIRST_FREE_OBJECTID = 256ULL`` and
``BTRFS_LAST_FREE_OBJECTID = -256ULL`` refer to file trees.

Otherwise, the objectid should be considered reserved for internal use.

-  BTRFS_ROOT_TREE_OBJECTID = 1

   The object id that refers to the ``ROOT_TREE`` itself.

-  BTRFS_EXTENT_TREE_OBJECTID = 2

   The objectid that refers to the ``EXTENT_TREE``

-  BTRFS_CHUNK_TREE_OBJECTID = 3

   The objectid that refers to the root of the ``CHUNK_TREE``

-  BTRFS_DEV_TREE_OBJECTID = 4

   The objectid that refers to the root of the ``DEV_TREE``

-  BTRFS_FS_TREE_OBJECTID = 5

   The objectid that refers to the global ``FS_TREE`` root.

-  BTRFS_CSUM_TREE_OBJECTID = 7

   The objectid that refers to the ``CSUM_TREE``

-  BTRFS_QUOTA_TREE_OBJECTID = 8

   The objectid that refers to the ``QUOTA_TREE``

-  BTRFS_UUID_TREE_OBJECTID = 9

   The objectid that refers to the ``UUID_TREE``.

-  BTRFS_FREE_SPACE_TREE_OBJECTID = 10

   The objectid that refers to the ``FREE_SPACE_TREE``.

-  BTRFS_TREE_LOG_OBJECTID = -7ULL

   The objectid that refers to the ``TREE_LOG`` tree.

-  BTRFS_TREE_RELOC_OBJECTID = -8ULL

   The objectid that refers to the ``TREE_RELOC`` tree.

-  BTRFS_DATA_RELOC_TREE_OBJECTID = -9ULL

   The objectid that refers to the ``DATA_RELOC`` tree.

The following are well-known objectids within the ``ROOT_TREE`` that do not
refer to other trees.

-  BTRFS_ROOT_TREE_DIR_OBJECTID = 6

   The objectid that refers to the directory within the root tree. If it
   exists, it will have the usual items used to implement a directory
   associated with it.  There will only be a single entry called ``default``
   that points to a key to be used as the root directory on the file system
   instead of the ``FS_TREE``.

-  BTRFS_ORPHAN_OBJECTID = -5ULL

   The objectid used for orphan root tracking.

Developer note: If implementing a feature that requires a new objectid in the
reserved range, you must reserve the objectid via the mailing list before
posting your code for general use. This is a disk format change.

Orphans

Removing a root is a multi-step process that may involve many transactions.
References to every extent used by the tree must be decremented and, if they
hit zero, the extents must be released. It is possible that the system crashes,
loses power, or otherwise encounters an error during root removal. Without
additional information, the file system could ultimately contain partially
removed roots, which would make it inconsistent. When a root is removed, it
performs several small operations in a single transaction in preparation for
removal. This process should be familiar to those with an understanding of how
orphans work when an inode is unlinked on any UNIX-style file system.

#. Unlink the root from the directory that contains it.
#. Initialize the ``drop_progress`` and
   ``drop_level`` fields and set the
   ``refs`` field to ``0`` in the
   ``ROOT_ITEM``.
#. If an orphan key for this root has not already been inserted into the tree, insert one.
#. Remove the UUID entries for this root and any associated received root from the
   ``UUID_TREE``.

Ultimately, the cleaner thread handles the reference count adjustments and,
once that is complete, the root has been successfully removed and it removes
the orphan key for that root. As the cleaner progresses, the ``drop_progress``
and ``drop_level`` fields are updated to reflect the most recently processed
item.

This process may be interrupted at any time and it must be recoverable. The
orphan key is how btrfs avoids inconsistencies when that occurs. The orphan key
is located in the ``ROOT_TREE`` and is of the following form.

+-----------------------------------+
| struct btrfs_key                  |
+===================================+
| ``objectid``                      |
+-----------------------------------+
| ``BTRFS_ORPHAN_OBJECTID [-5ULL]`` |
+-----------------------------------+

-  There is no item body associated with this key. All required information is
   contained within the key itself and the ``ROOT_ITEM`` associated with the
   objectid contained in ``offset``

When the file system is mounted again after failure, the ``ROOT_TREE`` is
searched for all orphan keys and the process is resumed for each one using the
``drop_progress`` and ``drop_level`` fields in the ``ROOT_ITEM``.

EXTENT tree (2)
^^^^^^^^^^^^^^^

TODO

-  Holds EXTENT_ITEMs, BLOCK_GROUP_ITEMs
-  Pointed to by ROOT


EMPTY_SUBVOL dir (2)
^^^^^^^^^^^^^^^^^^^^

TODO

CHUNK_TREE (3)
^^^^^^^^^^^^^^

The chunk tree holds all DEV_ITEMs and CHUNK_ITEMs, making it possible to
determine the device(s) and physical address(es) corresponding to a given
logical address. It is therefore crucial for access to the contents of the
filesystem.

The chunk tree resides entirely in SYSTEM block groups, and will therefore be
accessible from the CHUNK_ITEM array in the Superblock. It also has an entry in
the ROOT tree.


Reserved objectids
^^^^^^^^^^^^^^^^^^

-  BTRFS_FIRST_CHUNK_TREE_OBJECTID = 256

   This objectid indicates the first available objectid in this ``CHUNK_TREE``. In practice, it is
   the only objectid used in the tree. The ``offset`` field of the key is the only component used to
   distinguish separate :ref:`CHUNK_ITEM<CHUNK_ITEM_CONTENT>` items.


Dev tree (4)
^^^^^^^^^^^^

The dev tree holds all DEV_EXTENTs, making it possible to determine the logical
address corresponding to a given physical address. This is necessary when
shrinking or removing devices. The dev tree has an entry in the root tree.


FS_TREE (5)
^^^^^^^^^^^

TODO

-  Holds ``INODE_ITEM``,
   ``INODE_REF``,
   ``DIR_ITEM``, DIR_INDEXen, XATTR_ITEMs,
   ``EXTENT_DATA`` for a filesystem
-  Pointed to by ROOT
-  TODO: ".."


Root tree directory
^^^^^^^^^^^^^^^^^^^

The root tree directory is stored in the root tree. It has an INODE_ITEM and a
DIR_ITEM with name "default" pointing to the FS tree. There is also a
corresponding INODE_REF, but no DIR_INDEX. The objectid of the root tree
directory is stored in the superblock, but is currently always 6.


Checksum tree (7)
^^^^^^^^^^^^^^^^^

The checksum tree contains all the EXTENT_CSUMs. It has an entry in the root
tree.


ORPHAN (-5)
^^^^^^^^^^^

TODO


TREE_LOG (-6)
^^^^^^^^^^^^^

TODO


TREE_LOG_FIXUP (-7)
^^^^^^^^^^^^^^^^^^^

TODO


TREE_RELOC (-8)
^^^^^^^^^^^^^^^

TODO

-  Just a copy of another tree


DATA_RELOC tree (-9)
^^^^^^^^^^^^^^^^^^^^

TODO

-  Holds 100 INODE_ITEM 0
-  Holds 100 INODE_REF 100 0:'..'
-  Pointed to by ROOT


EXTENT_CSUM (-a)
^^^^^^^^^^^^^^^^

TODO


MULTIPLE_OBJECTIDS (-100)
^^^^^^^^^^^^^^^^^^^^^^^^^

TODO


Item Types
~~~~~~~~~~


INODE_ITEM (01)
^^^^^^^^^^^^^^^

Location
''''''''

``INODE_ITEM`` items are located primarily in file trees but are also found in the
ROOT_TREE to implement the free space cache (v1).

Usage
'''''

+---------------------------------+
| struct btrfs_key                |
+=================================+
| objectid                        |
+---------------------------------+
| objectid (Used as inode number) |
+---------------------------------+

Description
'''''''''''

Contains the stat information for an inode; see stat(2).


Item Contents
'''''''''''''

``INODE_ITEM`` items contain a single ``btrfs_inode_item`` structure.


INODE_REF (0c)
^^^^^^^^^^^^^^

(inode_id, directory_id) TODO

From an inode to a name in a directory.

======= ==== ===== ======================
Off     Size Type  Description
======= ==== ===== ======================
0x0     0x8  UINT  index in the directory
0x8     0x2  UINT  (*n*)
a       *n*  ASCII name in the directory
a+\ *n*
======= ==== ===== ======================

This structure can be repeated...?


INODE_EXTREF (0d)
^^^^^^^^^^^^^^^^^

(inode_id, hash of name [using directory object ID as seed]) TODO

From an inode to a name in a directory. Used if the regarding INODE_REF array
ran out of space.  *This item requires the EXTENDED_IREF feature.*

========== ==== ===== ======================
Off        Size Type  Description
========== ==== ===== ======================
0x0        0x8  UINT  directory object ID
0x8        0x8  UINT  index in the directory
0x10       0x2  UINT  (*n*)
0x12       *n*  ASCII name in the directory
0x12+\ *n*
========== ==== ===== ======================

This structure can be repeated...?

XATTR_ITEM (18)
^^^^^^^^^^^^^^^

Location
''''''''

``XATTR_ITEM`` items are only located in file trees.


Usage
'''''

+------------------------------+
| ``struct btrfs_key``         |
+==============================+
| objectid                     |
+------------------------------+
| ``objectid of owning inode`` |
+------------------------------+


Description
'''''''''''

``XATTR_ITEM`` items contain extended attributes. Each name is hashed using the
name hash and that value is used in the key for locating the entry quickly.
Each ``XATTR_ITEM`` item contains one or more extended attributes with names
represented by the same hash. All extended attributes that share the same name
hash must fit in a single leaf.


Item Contents
'''''''''''''

``XATTR_ITEM`` items consist of a series of one or more extended attribute
entries with names that share a hash value. Each entry consists of a
``btrfs_dir_item`` structure immediately followed by the name and the attribute
data. The length of each name is contained in ``btrfs_dir_item.name_len``.  The
data payload begins immediately after the name. The data payload length is
contained in ``btrfs_dir_item.data_len`` ``btrfs_dir_item.data_len.location``
is unused and must be zeroed. ``btrfs_dir_item.type`` contains a shorthand
value referring to the type of item to which an entry refers it must always be
be ``BTRFS_FT_XATTR`` when used to describe an extended attribute.

When there is more than one entry for a single hash value, the offset of each
entry must be calculating using the lengths of the preceding entries including
names and data.

For more details, please see: ``struct btrfs_dir_item`` and ```DIR_ITEM``.


VERITY_DESC (24)
^^^^^^^^^^^^^^^^


Location
''''''''

``VERITY_DESC`` items are located in the FS_TREE. TODO


VERITY_MERKLE (25)
^^^^^^^^^^^^^^^^^^


Location
''''''''

``VERITY_MERKLE`` items are located in the FS_TREE. TODO


ORPHAN_ITEM (30)
^^^^^^^^^^^^^^^^

(-5, objid of orphan inode) TODO

``   Empty.``


DIR_LOG_ITEM (3c)
^^^^^^^^^^^^^^^^^

(directory_id, first offset) TODO

| ``   The log is considered authoritative for ([first offset, end offset)]``
| ``    0  8 UINT   end offset``


DIR_LOG_INDEX (48)
^^^^^^^^^^^^^^^^^^

(directory_id, first offset) TODO

``   Same as DIR_LOG_ITEM.``


DIR_ITEM (54)
^^^^^^^^^^^^^

Location
''''''''

``DIR_ITEM`` items are only located in file trees.


Usage
'''''

+------------------------------+
| ``struct btrfs_key``         |
+==============================+
| objectid                     |
+------------------------------+
| ``objectid of owning inode`` |
+------------------------------+


Description
'''''''''''

``DIR_ITEM`` items contain directory entries. Each name is hashed using the
name hash and that value is used in the key for locating the entry quickly.
Each ``DIR_ITEM`` item contains one or more directory entries with names
represented by the same hash. All directory entries that share the same name
hash must fit in a single leaf.


Item Contents
'''''''''''''

``DIR_ITEM`` items consist of a series of one or more directory entries with
names that share a hash value. Each entry consists of a ``btrfs_dir_item``
structure immediately followed by the name. The length of each name is
contained in ``btrfs_dir_item.name_len``. The location of the item to which
this entry refers is contained in ``btrfs_dir_item.location`` and must refer to
a valid item in the same file tree.  ``btrfs_dir_item.type`` contains a
shorthand value referring to the type of item to which an entry refers. It will
never be ``BTRFS_FT_XATTR`` when used in a standard directory.
``btrfs_dir_item.data_len`` is unused and must be ``0``.

When there is more than one entry for a single hash value, the offset of each
entry must be calculating using the lengths of the preceding entries including
names.

For more details, please see: ``struct btrfs_dir_item``.


DIR_INDEX (60)
^^^^^^^^^^^^^^

(parent objectid, 60, index in parent)

Allows looking up an item in a directory by index. Indices start at 2 (because
of "." and ".."); removed files can cause "holes" in the index space.
DIR_INDEXen have the same contents as DIR_ITEM, but may contain only one entry.


EXTENT_DATA (6c)
^^^^^^^^^^^^^^^^

(inode id, 6c, offset in file) TODO

The contents of a file.

===== ==== ==== ======================================
Off   Size Type Description
===== ==== ==== ======================================
0x0   0x8  UINT generation
0x8   0x8  UINT (*n*) size of decoded extent
0x10  0x1  UINT compression (0=none, 1=zlib, 2=LZO)
0x11  0x1  UINT encryption (0=none)
0x12  0x2  UINT other encoding (0=none)
0x14  0x1  UINT type (0=inline, 1=regular, 2=prealloc)
0x15
===== ==== ==== ======================================

If the extent is inline, the remaining item bytes are the data bytes (*n* bytes
in case no compression/encryption/other encoding is used).

Otherwise, the structure continues:

+-------+------+------+---------------------------------------------------------------------------+
| Off   | Size | Type | Description                                                               |
+=======+======+======+===========================================================================+
| 0x15  | 0x8  | UINT | (*ea*) logical address of extent. If this is zero, the extent is sparse   |
|       |      |      | and consists of all zeroes.                                               |
+-------+------+------+---------------------------------------------------------------------------+
| 0x1d  | 0x8  | UINT | (*es*) size of extent                                                     |
+-------+------+------+---------------------------------------------------------------------------+
| 0x25  | 0x8  | UINT | (*o*) offset within the extent                                            |
+-------+------+------+---------------------------------------------------------------------------+
| 0x2d  | 0x8  | UINT | (*s*) logical number of bytes in file                                     |
+-------+------+------+---------------------------------------------------------------------------+
| 0x35  |      |      |                                                                           |
+-------+------+------+---------------------------------------------------------------------------+

*ea* and *es* must exactly match an EXTENT_ITEM. If the *es* bytes of data at
logical address *ea* are decoded, *n* bytes will result. The file's data
contains the *s* bytes at offset *o* within the decoded bytes. In the simplest,
uncompressed case, *o*\ =0 and *n*\ =\ *es*\ =\ *s*, so the file's data simply
contains the *n* bytes at logical address *ea*.


EXTENT_CSUM (80)
^^^^^^^^^^^^^^^^

(-a, logical address?) TODO

| ``   Contains one or more checksums of the type in the superblock for adjacent``
| ``   blocks starting at logical address (blocksize).``


ROOT_ITEM (84)
^^^^^^^^^^^^^^

Location
''''''''

``ROOT_ITEM`` items are only located in the `ROOT_TREE <#ROOT_TREE>`__.


Usage
'''''

+----------------------------------------------------------+
| ``struct btrfs_key``                                     |
+==========================================================+
| objectid                                                 |
+----------------------------------------------------------+
| ``objectid of root (TODO: document reserved objectids)`` |
+----------------------------------------------------------+


Description
'''''''''''

A fundamental component of btrfs is the btree. ``ROOT_ITEM`` items define the
location and parameters of the root of a btree.


Item Contents
'''''''''''''

``ROOT_ITEM`` items contain a single ``btrfs_root_item`` structure.


ROOT_BACKREF (90)
^^^^^^^^^^^^^^^^^

(subtree id, 90, tree id) TODO

Same content as `ROOT_REF <#ROOT_REF_.289c.29>`__.


ROOT_REF (9c)
^^^^^^^^^^^^^


Location
''''''''

``ROOT_REF`` items are only located in the ```ROOT_TREE`` <#ROOT_TREE>`__.

(tree id, subtree id) TODO

| ``    0  8 UINT   ID of directory in [tree id] that contains the subtree``
| ``    8  8 UINT   Sequence (index in tree) (even, starting at 2?)``
| ``   10  2 UINT   (n)``
| ``   12  n ASCII  name``


EXTENT_ITEM (a8)
^^^^^^^^^^^^^^^^

Location
''''''''

``EXTENT_ITEM`` items are only located in the ```EXTENT_TREE`` <#EXTENT_TREE>`__.


Usage
'''''

+-------------------------------------+
| ``struct btrfs_key``                |
+=====================================+
| objectid                            |
+-------------------------------------+
| ``byte offset for start of extent`` |
+-------------------------------------+


Description
'''''''''''

``EXTENT_ITEM`` items describe the space allocated for metadata tree nodes and
leafs as well as data extents. The space is allocated from block groups that
define the appropriate regions. In addition to functioning as basic allocation
records, ``EXTENT_ITEM`` items also contain back references that can be used to
repair the file system or resolve extent ownership back to a set of one or more
file trees. Although ``EXTENT_ITEM`` items can be used to describe both
``DATA`` and ``TREE_BLOCK`` extents, newer file systems with the skinny
metadata feature enabled at mkfs time use METADATA_ITEM  items to represent
metadata instead.


Item Contents
'''''''''''''

``EXTENT_ITEM`` items begin with the ```btrfs_extent_item``
<Data_Structures#btrfs_extent_item>`__ structure and are followed by records
that are defined by the ``flags`` field in that structure.


METADATA_ITEM (a9)
^^^^^^^^^^^^^^^^^^

Location
''''''''

``METADATA_ITEM`` items are only located in the ``EXTENT_TREE``.


Usage
'''''

+-------------------------------------+
| ``struct btrfs_key``                |
+=====================================+
| objectid                            |
+-------------------------------------+
| ``byte offset for start of extent`` |
+-------------------------------------+


Description
'''''''''''

``METADATA_ITEM`` items describe the space allocated for metadata tree nodes
and leafs. The space is allocated from block groups that define metadata
regions. In addition to functioning as basic allocation records,
``METADATA_ITEM`` items also contain back references that can be used to repair
the file system or resolve extent ownership back to a set of one or more file
trees.


Item Contents
'''''''''''''

``METADATA_ITEM`` items begin with the ``btrfs_extent_item`` structure and are
followed by records that are defined by the ``flags`` field in that structure.


TREE_BLOCK_REF (b0)
^^^^^^^^^^^^^^^^^^^

(logical address, b0, root object id) TODO

``    0   8 UINT   offset (the object ID of the tree)``


EXTENT_DATA_REF (b2)
^^^^^^^^^^^^^^^^^^^^

(logical address, b2, hash of first three fields) TODO

===== ==== ==== =======================================
Off   Size Type Description
===== ==== ==== =======================================
0x0   0x8  UINT root objectid (id of tree contained in)
0x8   0x8  UINT object id (owner)
0x10  0x8  UINT offset (in the file data)
0x18  0x4  UINT count (always 1?)
===== ==== ==== =======================================

EXTENT_REF_V0 (b4)
^^^^^^^^^^^^^^^^^^

TODO


SHARED_BLOCK_REF (b6)
^^^^^^^^^^^^^^^^^^^^^

(logical address, b6, parent) TODO

===== ==== ==== ===========
Off   Size Type Description
===== ==== ==== ===========
0x0   0x8  UINT offset
0x8
===== ==== ==== ===========


SHARED_DATA_REF (b8)
^^^^^^^^^^^^^^^^^^^^

(logical address, b8, parent) TODO

===== ==== ==== =================
Off   Size Type Description
===== ==== ==== =================
0x0   0x8  UINT offset
0x8   0x4  UINT count (always 1?)
0xc
===== ==== ==== =================


BLOCK_GROUP_ITEM (c0)
^^^^^^^^^^^^^^^^^^^^^


Location
''''''''

``BLOCK_GROUP_ITEM`` items are only found in the ``EXTENT_TREE``.


Usage
'''''

+---------------------------------------------------------------------------------+
| ``struct btrfs_key``                                                            |
+=================================================================================+
| objectid                                                                        |
+---------------------------------------------------------------------------------+
| Starting offset in the space defined by the ```EXTENT_TREE`` <#EXTENT_TREE>`__. |
+---------------------------------------------------------------------------------+


Description
'''''''''''

While the ``EXTENT_TREE`` defines the address space used for extent allocations
for the entire file system, block groups allocate and define the parameters
within that space. Every ``EXTENT_ITEM`` or ``METADATA_ITEM`` that describes an
extent in use by the file system is apportioned from allocated block groups.
Each block group can represent space used for ``SYSTEM`` objects (e.g. the
``CHUNK_TREE`` and primary super block), ``METADATA`` trees and items, or
``DATA`` extents. It is possible to combine ``METADATA`` and ``DATA``
allocations within a single block group, though it is not recommended.  This
mixed allocation policy is typically only seen on file systems smaller than
approximately 10 GiB in size.


Item Contents
'''''''''''''

``BTRFS_BLOCK_GROUP`` items contain a single
``struct btrfs_block_group_item``.


DEV_EXTENT (cc)
^^^^^^^^^^^^^^^

(device id, cc, physical address) TODO

Maps from physical address to logical.

===== ==== ===== =======================
Off   Size Type  Description
===== ==== ===== =======================
0x0   0x8  UINT  chunk tree (always 3)
0x8   0x8  OBJID chunk oid (always 256?)
0x10  0x8  UINT  logical address
0x18  0x8  UINT  size in bytes
0x20  0x10 UUID  chunk tree UUID
0x30
===== ==== ===== =======================


DEV_ITEM (d8)
^^^^^^^^^^^^^

(1, device id) TODO

Contains information about one device.

===== ==== ==== ==============================
Off   Size Type Description
===== ==== ==== ==============================
0x0   0x8  UINT device id
0x8   0x8  UINT number of bytes
0x10  0x8  UINT number of bytes used
0x18  0x4  UINT optimal I/O align
0x1c  0x4  UINT optimal I/O width
0x20  0x4  UINT minimal I/O size (sector size)
0x24  0x8  UINT type
0x2c  0x8  UINT generation
0x34  0x8  UINT start offset
0x3c  0x4  UINT dev group
0x40  0x1  UINT seek speed
0x41  0x1  UINT bandwidth
0x42  0x10 UUID device UUID
0x52  0x10 UUID FS UUID
0x62
===== ==== ==== ==============================


.. _CHUNK_ITEM_CONTENT:
CHUNK_ITEM (0xe4)
^^^^^^^^^^^^^^^

Key format: (100, logical address)

Maps logical address to physical.

==== ==== ===== ===========
Off  Size Type  Description
==== ==== ===== ===========
0x0  0x8  UINT  size of chunk (bytes)
0x8  0x8  OBJID root referencing this chunk(2)
0x10 0x8  UINT  stripe length
0x18 0x8  UINT  type (same as flags for block group?)
0x20 0x4  UINT  optimal io alignment
0x24 0x4  UINT  optimal io width
0x28 0x4  UINT  minimal io size (sector size)
0x2c 0x2  UINT  number of stripes
0x2e 0x2  UINT  sub stripes (only for RAID10,fixed 2)
0x30
==== ==== ===== ===========

Stripes follow (for each number of stripes):

==== ==== ===== ===========
Off  Size Type  Description
==== ==== ===== ===========
0x0  0x8  OBJID device id
0x8  0x8  UINT  physical offset of stripe on device
0x10 0x10 UUID  device UUID
0x20
==== ==== ===== ===========


STRING_ITEM (fd)
^^^^^^^^^^^^^^^^

(anything, 0)

Contains a string; used for testing only.
