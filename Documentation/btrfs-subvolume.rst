btrfs-subvolume(8)
==================

SYNOPSIS
--------

**btrfs subvolume** <subcommand> [<args>]

DESCRIPTION
-----------

:command:`btrfs subvolume` is used to create/delete/list/show btrfs subvolumes and
snapshots.

.. include:: ch-subvolume-intro.rst

SUBVOLUME AND SNAPSHOT
----------------------

A subvolume is a part of filesystem with its own independent
file/directory hierarchy. Subvolumes can share file extents. A snapshot is
also subvolume, but with a given initial content of the original subvolume.

.. note::
   A subvolume in btrfs is not like an LVM logical volume, which is block-level
   snapshot while btrfs subvolumes are file extent-based.

A subvolume looks like a normal directory, with some additional operations
described below. Subvolumes can be renamed or moved, nesting subvolumes is not
restricted but has some implications regarding snapshotting.

A subvolume in btrfs can be accessed in two ways:

* like any other directory that is accessible to the user
* like a separately mounted filesystem (options *subvol* or *subvolid*)

In the latter case the parent directory is not visible and accessible. This is
similar to a bind mount, and in fact the subvolume mount does exactly that.

A freshly created filesystem is also a subvolume, called *top-level*,
internally has an id 5. This subvolume cannot be removed or replaced by another
subvolume. This is also the subvolume that will be mounted by default, unless
the default subvolume has been changed (see subcommand :ref:`set-default<man-subvolume-set-default>`).

A snapshot is a subvolume like any other, with given initial content. By
default, snapshots are created read-write. File modifications in a snapshot
do not affect the files in the original subvolume.

SUBCOMMAND
-----------

create [options] [<dest>/]<name> [[<dest2>/]<name2> ...]
        Create subvolume(s) at the destination(s).

        If *dest* part of the path is not given, subvolume *name* will be
        created in the current directory.

        If multiple destinations are given, then the given options are applied to all
	subvolumes.

	If failure happens for any of the destinations, the command would
	still retry the remaining destinations, but would return 1 to indicate
	the failure (similar to what :command:`mkdir` would do.

        ``Options``

        -i <qgroupid>
                Add the newly created subvolume to a qgroup. This option can be given multiple
                times.

        -p|--parents
                Create any missing parent directories for each argument (like :command:`mkdir -p`).

delete [options] [<subvolume> [<subvolume>...]], delete -i|--subvolid <subvolid> <path>
        Delete the subvolume(s) from the filesystem.

        If *subvolume* is not a subvolume, btrfs returns an error but continues if
        there are more arguments to process.

        If *--subvolid* is used, *path* must point to a btrfs filesystem. See
        :ref:`btrfs subvolume list<man-subvolume-list>` or
        :ref:`btrfs inspect-internal rootid<man-inspect-rootid>`
        how to get the subvolume id.

        The corresponding directory is removed instantly but the data blocks are
        removed later in the background. The command returns immediately. See
        :ref:`btrfs subvolume sync<man-subvolume-sync>` how to wait until the subvolume gets completely removed.

        The deletion does not involve full transaction commit by default due to
        performance reasons.  As a consequence, the subvolume may appear again after a
        crash.  Use one of the *--commit* options to wait until the operation is
        safely stored on the device.

        Deleting subvolume needs sufficient permissions, by default the owner
        cannot delete it unless it's enabled by a mount option
        *user_subvol_rm_allowed*, or deletion is run as root.
        The default subvolume (see :ref:`btrfs subvolume set-default<man-subvolume-set-default>`)
        cannot be deleted and
        returns error (EPERM) and this is logged to the system log. A subvolume that's
        currently involved in send (see :doc:`btrfs-send`)
        also cannot be deleted until the
        send is finished. This is also logged in the system log.

        ``Options``

        -c|--commit-after
                wait for transaction commit at the end of the operation.

        -C|--commit-each
                wait for transaction commit after deleting each subvolume.

        -i|--subvolid <subvolid>
                subvolume id to be removed instead of the <path> that should point to the
                filesystem with the subvolume

        -R|--recursive
                delete subvolumes beneath each subvolume recursively

                This requires either `CAP_SYS_ADMIN` or the filesystem must be
                mounted with `user_subvol_rm_allowed` mount option.
                In the unprivileged case, subvolumes which cannot be accessed
                are skipped.  The deletion is not atomic.

        -v|--verbose
                (deprecated) alias for global *-v* option

find-new <subvolume> <last_gen>
        List the recently modified files in a subvolume, after *last_gen* generation.

get-default <path>
        Get the default subvolume of the filesystem *path*.

        The output format is similar to :command:`subvolume list` command.

.. _man-subvolume-list:

list [options] [-G [\+|-]<value>] [-C [+|-]<value>] [--sort=rootid,gen,ogen,path] <path>
        List the subvolumes present in the filesystem *path*.

        For every subvolume the following information is shown by default:

        ID *ID* gen *generation* top level *parent_ID* path *path*

        where *ID* is subvolume's (root)id, *generation* is an internal counter which is
        updated every transaction, *parent_ID* is the same as the parent subvolume's id,
        and *path* is the relative path of the subvolume to the top level subvolume.
        The subvolume's ID may be used by the subvolume set-default command,
        or at mount time via the *subvolid=* option.

        ``Options``

        Path filtering:

        -o
                Print only subvolumes below specified <path>. Note that this is not a
                recursive command, and won't show nested subvolumes under <path>.
        -a
                print all the subvolumes in the filesystem and distinguish between
                absolute and relative path with respect to the given *path*.

        Field selection:

        -p
                print the parent ID
                (*parent* here means the subvolume which contains this subvolume).
        -c
                print the ogeneration of the subvolume, aliases: ogen or origin generation.
        -g
                print the generation of the subvolume (default).
        -u
                print the UUID of the subvolume.
        -q
                print the parent UUID of the subvolume
                (*parent* here means subvolume of which this subvolume is a snapshot).
        -R
                print the UUID of the sent subvolume, where the subvolume is the result of a receive operation.

        Type filtering:

        -s
                only snapshot subvolumes in the filesystem will be listed.
        -r
                only readonly subvolumes in the filesystem will be listed.
        -d
                list deleted subvolumes that are not yet cleaned.

        Other:

        -t
                print the result as a table.

        Sorting:

        By default the subvolumes will be sorted by subvolume ID ascending.

        -G [+|-]<value>
                list subvolumes in the filesystem that its generation is
                >=, \<= or = value. \'\+' means >= value, \'-' means \<= value, If there is
                neither \'+' nor \'-', it means = value.
        -C [+|-]<value>
                list subvolumes in the filesystem that its ogeneration is
                >=, \<= or = value. The usage is the same to *-G* option.
        --sort=rootid,gen,ogen,path
                list subvolumes in order by specified items.
                you can add *+* or *-* in front of each items, *+* means ascending,
                *-* means descending. The default is ascending.

                for *--sort* you can combine some items together by *,*, just like
                *--sort=+ogen,-gen,path,rootid*.

.. _man-subvolume-set-default:

set-default [<subvolume>|<id> <path>]
        Set the default subvolume for the (mounted) filesystem.

        Set the default subvolume for the (mounted) filesystem at *path*. This will hide
        the top-level subvolume (i.e. the one mounted with *subvol=/* or *subvolid=5*).
        Takes action on next mount.

        There are two ways how to specify the subvolume, by *id* or by the *subvolume*
        path.
        The id can be obtained from :ref:`btrfs subvolume list<man-subvolume-list>`
        :ref:`btrfs subvolume show<man-subvolume-show>` or
        :ref:`btrfs inspect-internal rootid<man-inspect-rootid>`.

.. _man-subvolume-show:

show [options] <path>
        Show more information about a subvolume (UUIDs, generations, times, flags,
        related snapshots).

        .. code-block:: none

                /mnt/btrfs/subvolume
                        Name:                   subvolume
                        UUID:                   5e076a14-4e42-254d-ac8e-55bebea982d1
                        Parent UUID:            -
                        Received UUID:          -
                        Creation time:          2018-01-01 12:34:56 +0000
                        Subvolume ID:           79
                        Generation:             2844
                        Gen at creation:        2844
                        Parent ID:              5
                        Top level ID:           5
                        Flags:                  -
                        Snapshot(s):

        ``Options``

        -r|--rootid <ID>
                show details about subvolume with root *ID*, looked up in *path*
        -u|--uuid UUID
                show details about subvolume with the given *UUID*, looked up in *path*

snapshot [-r] [-i <qgroupid>] <source> <dest>|[<dest>/]<name>
        Create a snapshot of the subvolume *source* with the
        name *name* in the *dest* directory.

        If only *dest* is given, the subvolume will be named the basename of *source*.
        If *source* is not a subvolume, btrfs returns an error.

        ``Options``

        -r
                Make the new snapshot read only.
        -i <qgroupid>
                Add the newly created subvolume to a qgroup. This option can be given multiple
                times.

.. _man-subvolume-sync:

sync <path> [subvolid...]
        Wait until given subvolume(s) are completely removed from the filesystem after
        deletion. If no subvolume id is given, wait until all current deletion requests
        are completed, but do not wait for subvolumes deleted in the meantime.

        If the filesystem status changes to read-only then the waiting is interrupted.

        ``Options``

        -s <N>
                sleep N seconds between checks (default: 1)

EXAMPLES
--------

Deleting a subvolume
^^^^^^^^^^^^^^^^^^^^

If we want to delete a subvolume called *foo* from a btrfs volume mounted at
:file:`/mnt/bar` we could run the following:

.. code-block:: bash

        btrfs subvolume delete /mnt/bar/foo

EXIT STATUS
-----------

**btrfs subvolume** returns a zero exit status if it succeeds. A non-zero value is
returned in case of failure.

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.  Please refer to the documentation at
`https://btrfs.readthedocs.io <https://btrfs.readthedocs.io>`_.

SEE ALSO
--------

:doc:`btrfs-qgroup`,
:doc:`btrfs-quota`,
:doc:`btrfs-send`,
:doc:`mkfs.btrfs`,
:manref:`mount(8)`
