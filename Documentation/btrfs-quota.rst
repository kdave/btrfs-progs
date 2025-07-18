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

The qgroup implementation is considered reasonably stable for daily use and has
been enabled in various distributions.

When quotas are activated, they affect all extent processing, which takes a
performance hit. Activation of qgroups is not recommended unless the user
intends to actually use them.

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
