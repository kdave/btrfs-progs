fsck.btrfs(8)
=============

SYNOPSIS
--------

**fsck.btrfs** [-aApy] [<device>...]

DESCRIPTION
-----------

**fsck.btrfs** is a type of utility that should exist for any filesystem and is
called during system setup when the corresponding ``/etc/fstab`` entries
contain non-zero value for *fs_passno*, see ``fstab(5)`` for more.

Traditional filesystems need to run their respective fsck utility in case the
filesystem was not unmounted cleanly and the log needs to be replayed before
mount. This is not needed for BTRFS. You should set fs_passno to 0.

If you wish to check the consistency of a BTRFS filesystem or repair a damaged
filesystem, see ``btrfs-check(8)``. By default filesystem consistency is checked,
the repair mode is enabled via the *--repair* option (use with care!).

OPTIONS
-------

The options are all the same and detect if **fsck.btrfs** is executed in
non-interactive mode and exits with success, otherwise prints a message about
btrfs check.

EXIT STATUS
-----------

There are two possible exit codes returned:

0
        No error

8
        Operational error, eg. device does not exist

FILES
-----

``/etc/fstab``

SEE ALSO
--------

``btrfs(8)``,
``fsck(8)``,
``fstab(5)``
