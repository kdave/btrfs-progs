Experimental features
=====================

User space tools (btrfs-progs)
------------------------------

Experimental or unstable features may be enabled by

    ./configure --enable-experimental

but as it says, the interface, command names, output formatting should be considered
unstable and not for production use. However testing is welcome and feedback or bugs
filed as issues.

In the code use it like:

.. code-block:: none

    if (EXPERIMENTAL) {
        ...
    }

in case it does not interfere with other code or does not depend on an `#if`
where it would break default build.

Or:

.. code-block:: none

    #if EXPERIMENTAL
    ...
    #endif

for larger code blocks.

.. note::
   Do not use `#ifdef` as the macro is always defined so this would not work as
   expected.

Each feature should be tracked in an issue with label **experimental** (list of
active issues https://github.com/kdave/btrfs-progs/labels/experimental), with a
description and a TODO list items. Individual tasks can be tracked in other
issues if needed.


Kernel module
-------------

The kernel module can be configured to enable experimental features or
functionality since version 6.13 by
`CONFIG_BTRFS_EXPERIMENTAL <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/fs/btrfs/Kconfig#n82>`__ .
The features can get added or removed in each release. At runtime the status
can be seen in the system log message once the kernel module is loaded:

.. code-block:: none

   Btrfs loaded, experimental=on, debug=on, assert=on, ref-verify=on, zoned=yes, fsverity=yes


In some cases (standalone features) it's exported in :file:`/sys/fs/btrfs/FSID/features`.
