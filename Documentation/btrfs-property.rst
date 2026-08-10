btrfs-property(8)
=================

SYNOPSIS
--------

**btrfs property** <subcommand> <args>

DESCRIPTION
-----------

**btrfs property** is used to get/set/list property for given filesystem object.
The object can be an inode (file or directory), subvolume or the whole
filesystem.

**btrfs property** provides an unified and user-friendly method to tune different
btrfs properties instead of using the traditional method like :manref:`chattr(1)` or
:manref:`lsattr(1)`.

Object types
^^^^^^^^^^^^

A property might apply to several object types so in some cases it's necessary
to specify that explicitly, however it's not needed in the most common case of
files and directories.

The subcommands take parameter *-t*, use first letter as a shortcut (*f/s/d/i*)
of the type:

- filesystem
- subvolume
- device
- inode (file or directory)

Inode properties
^^^^^^^^^^^^^^^^

compression
        file data compression of an inode. The type and level are specified as *<TYPE>[=<LEVEL>]*
        using the same format as the *compress* mount option, described in
        :ref:`MOUNT OPTIONS<man-btrfs5-mount-options>` (level support since: 7.X).

        For more information on btrfs compression, see :doc:`Compression`.

        *""* (empty string) sets the default value (use compression specified by mount options).

        .. note::
             This has changed in version 5.18 of btrfs-progs and
             requires kernel 5.14 or newer to work.

        TYPE can be one of *zlib*, *lzo*, or *zstd* to select a specific algorithm, or *no* or *none*
        to disable compression. LEVEL can be in the range [1, 9] for *zlib* or [-15, 15] for *zstd*.
        If level is omitted or set to 0, the level specified via mount options will be used if
        the type matches. Otherwise, the type's default compression level will be used.

        .. note::
            When the filesystem is mounted using a kernel older than 7.X, the level set in the
            property will be ignored.

Subvolume properties
^^^^^^^^^^^^^^^^^^^^

ro
        read-only flag of subvolume: true or false. Please also see section *SUBVOLUME FLAGS*
        in :doc:`btrfs-subvolume` for possible implications regarding incremental send.

Filesystem properties
^^^^^^^^^^^^^^^^^^^^^

label
        label of the filesystem. For an unmounted filesystem, provide a path to a block
        device as object. For a mounted filesystem, specify a mount point.

SUBCOMMAND
----------

get [-t <type>] <object> [<name>]
        Read value of a property *name* of btrfs *object* of given *type*,
        empty *name* will read all of them

list [-t <type>] <object>
        List available properties with their descriptions for the given object.

.. _man-property-set:

set [-f] [-t <type>] <object> <name> <value>
        Set *value* of property *name* on a given btrfs object.

        ``Options``

        -f
                Force the change. Changing some properties may involve safety checks or
                additional changes that depend on the properties semantics.

EXAMPLES
--------

Set compression on a file:

.. code-block:: bash

   $ touch file1
   $ btrfs prop get file1
   [ empty output ]
   $ btrfs prop set file1 compression zstd
   $ btrfs prop get file1
   compression=zstd

Make a writeable subvolume read-only:

.. code-block:: bash

   $ btrfs subvol create subvol1
   [ fill subvol1 with data ]
   $ btrfs prop get subvol1
   ro=false
   $ btrfs prop set subvol1 ro true
   ro=true

EXIT STATUS
-----------

**btrfs property** returns a zero exit status if it succeeds. Non zero is
returned in case of failure.

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.  Please refer to the documentation at
`https://btrfs.readthedocs.io <https://btrfs.readthedocs.io>`_.

SEE ALSO
--------

:doc:`mkfs.btrfs`,
:manref:`lsattr(1)`,
:manref:`chattr(1)`
