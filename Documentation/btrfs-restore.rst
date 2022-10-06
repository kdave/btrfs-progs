btrfs-restore(8)
================

SYNOPSIS
--------

**btrfs restore** [options] <device> <path> | -l <device>

DESCRIPTION
-----------

**btrfs restore** is used to try to salvage files from a damaged filesystem and
restore them into *path* or just list the subvolume tree roots. The filesystem
image is not modified.

If the filesystem is damaged and cannot be repaired by the other tools
(:doc:`btrfs-check(8)<btrfs-check>` or :doc:`btrfs-rescue(8)<btrfs-rescue>`), **btrfs restore** could be used to
retrieve file data, as far as the metadata are readable. The checks done by
restore are less strict and the process is usually able to get far enough to
retrieve data from the whole filesystem. This comes at a cost that some data
might be incomplete or from older versions if they're available.

There are several options to attempt restoration of various file metadata type.
You can try a dry run first to see how well the process goes and use further
options to extend the set of restored metadata.

For images with damaged tree structures, there are several options to point the
process to some spare copy.

.. note::
        It is recommended to read the following btrfs wiki page if your data is
        not salvaged with default option:

        https://btrfs.wiki.kernel.org/index.php/Restore

OPTIONS
-------

-s|--snapshots
        get also snapshots that are skipped by default

-x|--xattr
        get extended attributes

-m|--metadata
        restore owner, mode and times for files and directories

-S|--symlinks
        restore symbolic links as well as normal files

-i|--ignore-errors
        ignore errors during restoration and continue

-o|--overwrite
        overwrite directories/files in *path*, eg. for repeated runs

-t <bytenr>
        use *bytenr* to read the root tree

-f <bytenr>
        only restore files that are under specified subvolume root pointed by *bytenr*

-u|--super <mirror>
        use given superblock mirror identified by <mirror>, it can be 0,1 or 2

-r|--root <rootid>
        only restore files that are under a specified subvolume whose objectid is *rootid*

-d
        find directory

-l|--list-roots
        list subvolume tree roots, can be used as argument for *-r*

-D|--dry-run
        dry run (only list files that would be recovered)

--path-regex <regex>
        restore only filenames matching a regular expression (``regex(7)``)
        with a mandatory format

        ``^/(|home(|/username(|/Desktop(|/.*))))$``

        The format is not very comfortable and restores all files in the
        directories in the whole path, so this is not useful for restoring
        single file in a deep hierarchy.

-c
        ignore case (*--path-regex* only)

-v|--verbose
        (deprecated) alias for global *-v* option

``Global options``

-v|--verbose
        be verbose and print what is being restored

EXIT STATUS
-----------

**btrfs restore** returns a zero exit status if it succeeds. Non zero is
returned in case of failure.

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.  Please refer to the documentation at
https://btrfs.readthedocs.io or wiki http://btrfs.wiki.kernel.org for further
information.

SEE ALSO
--------

:doc:`btrfs-check(8)<btrfs-check>`,
:doc:`btrfs-rescue(8)<btrfs-rescue>`,
:doc:`mkfs.btrfs(8)<mkfs.btrfs>`
