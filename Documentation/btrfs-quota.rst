btrfs-quota(8)
==============

SYNOPSIS
--------

**btrfs quota** <subcommand> <args>

DESCRIPTION
-----------

The commands under :command:`btrfs quota` are used to affect the global status of quotas
of a btrfs filesystem. The quota groups (qgroups) are managed by the subcommand
:doc:`btrfs-qgroup`.

.. note::
        Qgroups are different than the traditional user quotas and designed
        to track shared and exclusive data per-subvolume.  Please refer to the section
        :ref:`HIERARCHICAL QUOTA GROUP CONCEPTS<man-quota-hierarchical-quota-group-concepts>`
        for a detailed description.

STABILITY AND PERFORMANCE IMPLICATIONS
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The qgroup mode is considered not recommended for daily usage, unless there is
no planned new snapshot/subvolume creation and deletion.

The core design of qgroup mode and snapshot are not compatible from day one, and
are the cause of all kinds of performance problems.

When qgroup mode is activated, it affects all extent processing, which takes a
performance hit. Operations that modify a whole subvolume/snapshot in one go,
which include snapshot creation and subvolume deletion, are heavily affected and
can cause a system hang due to the heavy load.

Although the kernel is taking several workarounds, the problem is not fully resolved
and has extra costs, e.g. marking qgroup inconsistent, breaking limits and
requiring extra rescan.

Activation of qgroups is not recommended unless the user intends to actually use them,
and the usage does not involve new subvolume/snapshot.

.. _man-quota-hierarchical-quota-group-concepts:

HIERARCHICAL QUOTA GROUP CONCEPTS
---------------------------------

.. include:: ch-quota-intro.rst

SUBCOMMAND
----------

disable <path>
        Disable subvolume quota support for a filesystem.

enable [options] <path>
        Enable subvolume quota support for a filesystem. At this point it's
        possible the two modes of accounting. The *full* means that extent
        ownership by subvolumes will be tracked all the time, *simple* will
        account everything to the first owner. See the section for more details.

        ``Options``

	-s|--simple
		use simple quotas (squotas) instead of full qgroup accounting

rescan [options] <path>
        Trash all qgroup numbers and scan the metadata again with the current config.

        ``Options``

        -s|--status
                show status of a running rescan operation.
        -w|--wait
                start rescan and wait for it to finish (can be already in progress)
        -W|--wait-norescan
                wait for rescan to finish without starting it

status [options] <path>
        Print status information about quotas if enabled on *path*. The information
        is read from :file:`/sys/fs/btrfs/FSID/qgroups` and root privileges are
        not needed.

        Example output for quotas enabled by :command:`btrfs quota enable /mnt`:

        .. code-block:: none

		Quotas on /mnt:
		  Enabled:                 yes
		  Mode:                    qgroup (full accounting)
		  Inconsistent:            no
		  Override limits:         no
		  Drop subtree threshold:  3
		  Total count:             1
		  Level 0:                 1

        ``Options``

        --is-enabled
                only check if quotas are enabled, not not print anything

EXIT STATUS
-----------

**btrfs quota** returns a zero exit status if it succeeds. Non zero is
returned in case of failure.

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.  Please refer to the documentation at
`https://btrfs.readthedocs.io <https://btrfs.readthedocs.io>`_.

SEE ALSO
--------

:doc:`btrfs-qgroup`,
:doc:`btrfs-subvolume`,
:doc:`mkfs.btrfs`
