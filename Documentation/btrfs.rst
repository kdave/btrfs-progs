btrfs(8)
========

SYNOPSIS
--------

**btrfs** [global] <group> [<group>...] <command> [<args>]

DESCRIPTION
-----------

The :command:`btrfs` utility is a toolbox for managing btrfs filesystems.  There are
command groups to work with subvolumes, devices, for whole filesystem or other
specific actions. See section :ref:`COMMANDS<man-btrfs8-commands>`.

There are also standalone tools for some tasks like :doc:`btrfs-convert` or
:doc:`btrfstune` that were separate historically and/or haven't been merged to the
main utility. See section :ref:`STANDALONE TOOLS<man-btrfs8-standalone-tools>`
for more details.

For other topics (mount options, etc) please refer to the separate manual
page :doc:`btrfs-man5`.

COMMAND SYNTAX
--------------

Any command name can be shortened so long as the shortened form is unambiguous,
however, it is recommended to use full command names in scripts.  All command
groups have their manual page named **btrfs-<group>**.

For example: it is possible to run :command:`btrfs sub snaps` instead of
:command:`btrfs subvolume snapshot`.
But :command:`btrfs file s` is not allowed, because :command:`file s` may be interpreted
both as :command:`filesystem show` and as :command:`filesystem sync`.

If the command name is ambiguous, the list of conflicting options is
printed.

*Sizes*, both upon input and output, can be expressed in either SI or IEC-I
units (see :manref:`numfmt(1)`)
with the suffix `B` appended.
All numbers will be formatted according to the rules of the `C` locale
(ignoring the shell locale, see :manref:`locale(7)`).

For an overview of a given command use :command:`btrfs command --help`
or :command:`btrfs [command...] --help --full` to print all available options.

There are global options that are passed between *btrfs* and the *group* name
and affect behaviour not specific to the command, e.g. verbosity or the type
of the output.

--format <format>
        if supported by the command, print subcommand output in that format (text, json)

-v|--verbose
        increase verbosity of the subcommand

-q|--quiet
        print only errors

--log <level>
        set log level (default, info, verbose, debug, quiet)

The remaining options are relevant only for the main tool:

--help
        print condensed help for all subcommands

--version
        print version string

.. _man-btrfs8-commands:

COMMANDS
--------

balance
	Balance btrfs filesystem chunks across single or several devices.
	See :doc:`btrfs-balance` for details.

check
	Do off-line check on a btrfs filesystem.
	See :doc:`btrfs-check` for details.

device
	Manage devices managed by btrfs, including add/delete/scan and so
	on.  See :doc:`btrfs-device` for details.

filesystem
	Manage a btrfs filesystem, including label setting/sync and so on.
        See :doc:`btrfs-filesystem` for details.

inspect-internal
	Debug tools for developers/hackers.
	See :doc:`btrfs-inspect-internal` for details.

property
	Get/set a property from/to a btrfs object.
	See :doc:`btrfs-property` for details.

qgroup
	Manage quota group(qgroup) for btrfs filesystem.
	See :doc:`btrfs-qgroup` for details.

quota
	Manage quota on btrfs filesystem like enabling/rescan and etc.
	See :doc:`btrfs-quota` and :doc:`btrfs-qgroup` for details.

receive
	Receive subvolume data from stdin/file for restore and etc.
	See :doc:`btrfs-receive` for details.

replace
	Replace btrfs devices.
	See :doc:`btrfs-replace` for details.

rescue
	Try to rescue damaged btrfs filesystem.
	See :doc:`btrfs-rescue` for details.

restore
	Try to restore files from a damaged btrfs filesystem.
	See :doc:`btrfs-restore` for details.

scrub
	Scrub a btrfs filesystem.
	See :doc:`btrfs-scrub` for details.

send
	Send subvolume data to stdout/file for backup and etc.
	See :doc:`btrfs-send` for details.

subvolume
	Create/delete/list/manage btrfs subvolume.
	See :doc:`btrfs-subvolume` for details.

.. _man-btrfs8-standalone-tools:

STANDALONE TOOLS
----------------

New functionality could be provided using a standalone tool. If the functionality
proves to be useful, then the standalone tool is declared obsolete and its
functionality is copied to the main tool. Obsolete tools are removed after a
long (years) depreciation period.

Tools that are still in active use without an equivalent in :command:`btrfs`:

btrfs-convert
        in-place conversion from ext2/3/4 filesystems to btrfs
btrfstune
        tweak some filesystem properties on a unmounted filesystem
btrfs-select-super
        rescue tool to overwrite primary superblock from a spare copy
btrfs-find-root
        rescue helper to find tree roots in a filesystem

For space-constrained environments, it's possible to build a single binary with
functionality of several standalone tools. This is following the concept of
busybox where the file name selects the functionality. This works for symlinks
or hardlinks. The full list can be obtained by :command:`btrfs help --box`.

EXIT STATUS
-----------

**btrfs** returns a zero exit status if it succeeds. Non zero is returned in
case of failure.

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.  Please refer to the documentation at
`https://btrfs.readthedocs.io <https://btrfs.readthedocs.io>`_.

SEE ALSO
--------

:doc:`btrfs-man5`,
:doc:`btrfs-balance`,
:doc:`btrfs-check`,
:doc:`btrfs-convert`,
:doc:`btrfs-device`,
:doc:`btrfs-filesystem`,
:doc:`btrfs-inspect-internal`,
:doc:`btrfs-property`,
:doc:`btrfs-qgroup`,
:doc:`btrfs-quota`,
:doc:`btrfs-receive`,
:doc:`btrfs-replace`,
:doc:`btrfs-rescue`,
:doc:`btrfs-restore`,
:doc:`btrfs-scrub`,
:doc:`btrfs-send`,
:doc:`btrfs-subvolume`,
:doc:`btrfstune`,
:doc:`mkfs.btrfs`
