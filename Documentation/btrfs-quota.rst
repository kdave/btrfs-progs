btrfs-quota(8)
==============

SYNOPSIS
--------

**btrfs quota** <subcommand> <args>

DESCRIPTION
-----------

The commands under **btrfs quota** are used to affect the global status of quotas
of a btrfs filesystem. The quota groups (qgroups) are managed by the subcommand
:doc:`btrfs-qgroup(8)<btrfs-qgroup>`.

.. note::
        Qgroups are different than the traditional user quotas and designed
        to track shared and exclusive data per-subvolume.  Please refer to the section
        *HIERARCHICAL QUOTA GROUP CONCEPTS* for a detailed description.

PERFORMANCE IMPLICATIONS
^^^^^^^^^^^^^^^^^^^^^^^^

When quotas are activated, they affect all extent processing, which takes a
performance hit. Activation of qgroups is not recommended unless the user
intends to actually use them.

STABILITY STATUS
^^^^^^^^^^^^^^^^

The qgroup implementation has turned out to be quite difficult as it affects
the core of the filesystem operation. Qgroup users have hit various corner cases
over time, such as incorrect accounting or system instability. The situation is
gradually improving and issues found and fixed.

HIERARCHICAL QUOTA GROUP CONCEPTS
---------------------------------

.. include:: ch-quota-intro.rst

SUBCOMMAND
----------

disable <path>
        Disable subvolume quota support for a filesystem.

enable <path>
        Enable subvolume quota support for a filesystem.

rescan [options] <path>
        Trash all qgroup numbers and scan the metadata again with the current config.

        ``Options``

        -s|--status
                show status of a running rescan operation.
        -w|--wait
                start rescan and wait for it to finish (can be already in progress)
        -W|--wait-norescan
                wait for rescan to finish without starting it

EXIT STATUS
-----------

**btrfs quota** returns a zero exit status if it succeeds. Non zero is
returned in case of failure.

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.  Please refer to the documentation at
https://btrfs.readthedocs.io or wiki http://btrfs.wiki.kernel.org for further
information.

SEE ALSO
--------

:doc:`btrfs-qgroup(8)<btrfs-qgroup>`,
:doc:`btrfs-subvolume(8)<btrfs-subvolume>`,
:doc:`mkfs.btrfs(8)<mkfs.btrfs>`
