Experimental features
=====================

Experimental or unstable features may be enabled by

    ./configure --enable-experimental

but as it says, the interface, command names, output formatting should be considered
unstable and not for production use. However testing is welcome and feedback or bugs
filed as issues.

In the code use it like:

.. code-block::

    if (EXPERIMENTAL) {
        ...
    }

in case it does not interfere with other code or does not depend on an `#if`
where it would break default build.

Or:

.. code-block::

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
