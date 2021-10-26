btrfs-property(8)
=================

SYNOPSIS
--------

**btrfs property** <subcommand> <args>

DESCRIPTION
-----------

**btrfs property** is used to get/set/list property for given filesystem object.
The object can be an inode (file or directory), subvolume or the whole
filesystem.  See the description of **get** subcommand for more information about
both btrfs object and property.

***btrfs property** provides an unified and user-friendly method to tune different
btrfs properties instead of using the traditional method like ``chattr(1)`` or
``lsattr(1)``.

SUBCOMMAND
----------

get [-t <type>] <object> [<name>]
        get property from a btrfs *object* of given *type*

        A btrfs object, which is set by *object*, can be a btrfs filesystem
        itself, a btrfs subvolume, an inode (file or directory) inside btrfs,
        or a device on which a btrfs exists.

        The option *-t* can be used to explicitly
        specify what type of object you meant. This is only needed when a
        property could be set for more then one object type.

        Possible types are *s[ubvol]*, *f[ilesystem]*, *i[node]* and *d[evice]*, where
        the first lettes is a shortcut.

        Set the name of property by *name*. If no *name* is specified,
        all properties for the given object are printed. *name* is one of
        the following:

        ro
                read-only flag of subvolume: true or false. Please also see section *SUBVOLUME FLAGS*
                in ``btrfs-subvolume(8)`` for possible implications regarding incremental send.
        label
                label of the filesystem. For an unmounted filesystem, provide a path to a block
                device as object. For a mounted filesystem, specify a mount point.
        compression
                compression algorithm set for an inode, possible values: *lzo*, *zlib*, *zstd*.
                To disable compression use "" (empty string), *no* or *none*.

list [-t <type>] <object>
        Lists available properties with their descriptions for the given object.

        See the description of **get** subcommand for the meaning of each option.

set [-f] [-t <type>] <object> <name> <value>
        Sets a property on a btrfs object.

        See the description of **get** subcommand for the meaning of each option.

        ``Options``

        -f
                Force the change. Changing some properties may involve safety checks or
                additional changes that depend on the properties semantics.

EXIT STATUS
-----------

**btrfs property** returns a zero exit status if it succeeds. Non zero is
returned in case of failure.

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.
Please refer to the btrfs wiki http://btrfs.wiki.kernel.org for
further details.

SEE ALSO
--------

``mkfs.btrfs(8)``,
``lsattr(1)``,
``chattr(1)``
