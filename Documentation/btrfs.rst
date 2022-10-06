btrfs(8)
========

SYNOPSIS
--------

**btrfs** <command> [<args>]

DESCRIPTION
-----------

The **btrfs** utility is a toolbox for managing btrfs filesystems.  There are
command groups to work with subvolumes, devices, for whole filesystem or other
specific actions. See section *COMMANDS*.

There are also standalone tools for some tasks like **btrfs-convert** or
**btrfstune** that were separate historically and/or haven't been merged to the
main utility. See section *STANDALONE TOOLS* for more details.

For other topics (mount options, etc) please refer to the separate manual
page :doc:`btrfs(5)<btrfs-man5>`.

COMMAND SYNTAX
--------------

Any command name can be shortened so long as the shortened form is unambiguous,
however, it is recommended to use full command names in scripts.  All command
groups have their manual page named **btrfs-<group>**.

For example: it is possible to run **btrfs sub snaps** instead of
**btrfs subvolume snapshot**.
But **btrfs file s** is not allowed, because **file s** may be interpreted
both as **filesystem show** and as **filesystem sync**.

If the command name is ambiguous, the list of conflicting options is
printed.

For an overview of a given command use **btrfs command --help**
or **btrfs [command...] --help --full** to print all available options.

COMMANDS
--------

balance
	Balance btrfs filesystem chunks across single or several devices.
	See :doc:`btrfs-balance(8)<btrfs-balance>` for details.

check
	Do off-line check on a btrfs filesystem.
	See :doc:`btrfs-check(8)<btrfs-check>` for details.

device
	Manage devices managed by btrfs, including add/delete/scan and so
	on.  See :doc:`btrfs-device(8)<btrfs-device>` for details.

filesystem
	Manage a btrfs filesystem, including label setting/sync and so on.
        See :doc:`btrfs-filesystem(8)<btrfs-filesystem>` for details.

inspect-internal
	Debug tools for developers/hackers.
	See :doc:`btrfs-inspect-internal(8)<btrfs-inspect-internal>` for details.

property
	Get/set a property from/to a btrfs object.
	See :doc:`btrfs-property(8)<btrfs-property>` for details.

qgroup
	Manage quota group(qgroup) for btrfs filesystem.
	See :doc:`btrfs-qgroup(8)<btrfs-qgroup>` for details.

quota
	Manage quota on btrfs filesystem like enabling/rescan and etc.
	See :doc:`btrfs-quota(8)<btrfs-quota>` and :doc:`btrfs-qgroup(8)<btrfs-qgroup>` for details.

receive
	Receive subvolume data from stdin/file for restore and etc.
	See :doc:`btrfs-receive(8)<btrfs-receive>` for details.

replace
	Replace btrfs devices.
	See :doc:`btrfs-replace(8)<btrfs-replace>` for details.

rescue
	Try to rescue damaged btrfs filesystem.
	See :doc:`btrfs-rescue(8)<btrfs-rescue>` for details.

restore
	Try to restore files from a damaged btrfs filesystem.
	See :doc:`btrfs-restore(8)<btrfs-restore>` for details.

scrub
	Scrub a btrfs filesystem.
	See :doc:`btrfs-scrub(8)<btrfs-scrub>` for details.

send
	Send subvolume data to stdout/file for backup and etc.
	See :doc:`btrfs-send(8)<btrfs-send>` for details.

subvolume
	Create/delete/list/manage btrfs subvolume.
	See :doc:`btrfs-subvolume(8)<btrfs-subvolume>` for details.

STANDALONE TOOLS
----------------

New functionality could be provided using a standalone tool. If the functionality
proves to be useful, then the standalone tool is declared obsolete and its
functionality is copied to the main tool. Obsolete tools are removed after a
long (years) depreciation period.

Tools that are still in active use without an equivalent in **btrfs**:

btrfs-convert
        in-place conversion from ext2/3/4 filesystems to btrfs
btrfstune
        tweak some filesystem properties on a unmounted filesystem
btrfs-select-super
        rescue tool to overwrite primary superblock from a spare copy
btrfs-find-root
        rescue helper to find tree roots in a filesystem

Deprecated and obsolete tools:

btrfs-debug-tree
        moved to **btrfs inspect-internal dump-tree**. Removed from
        source distribution.
btrfs-show-super
        moved to **btrfs inspect-internal dump-super**, standalone
        removed.
btrfs-zero-log
        moved to **btrfs rescue zero-log**, standalone removed.

For space-constrained environments, it's possible to build a single binary with
functionality of several standalone tools. This is following the concept of
busybox where the file name selects the functionality. This works for symlinks
or hardlinks. The full list can be obtained by **btrfs help --box**.

EXIT STATUS
-----------

**btrfs** returns a zero exit status if it succeeds. Non zero is returned in
case of failure.

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.  Please refer to the documentation at
https://btrfs.readthedocs.io or wiki http://btrfs.wiki.kernel.org for further
information.

SEE ALSO
--------

:doc:`btrfs(5)<btrfs-man5>`,
:doc:`btrfs-balance(8)<btrfs-balance>`,
:doc:`btrfs-check(8)<btrfs-check>`,
:doc:`btrfs-convert(8)<btrfs-convert>`,
:doc:`btrfs-device(8)<btrfs-device>`,
:doc:`btrfs-filesystem(8)<btrfs-filesystem>`,
:doc:`btrfs-inspect-internal(8)<btrfs-inspect-internal>`,
:doc:`btrfs-property(8)<btrfs-property>`,
:doc:`btrfs-qgroup(8)<btrfs-qgroup>`,
:doc:`btrfs-quota(8)<btrfs-quota>`,
:doc:`btrfs-receive(8)<btrfs-receive>`,
:doc:`btrfs-replace(8)<btrfs-replace>`,
:doc:`btrfs-rescue(8)<btrfs-rescue>`,
:doc:`btrfs-restore(8)<btrfs-restore>`,
:doc:`btrfs-scrub(8)<btrfs-scrub>`,
:doc:`btrfs-send(8)<btrfs-send>`,
:doc:`btrfs-subvolume(8)<btrfs-subvolume>`,
:doc:`btrfstune(8)<btrfstune>`,
:doc:`mkfs.btrfs(8)<mkfs.btrfs>`
