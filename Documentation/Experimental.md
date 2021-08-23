Experimental features
=====================

Experimental or unstable features may be enabled by

    ./configure --enable-experimental

but as it says, the interface, command names, output formatting should be considered
unstable and not for production use. However testing is welcome and feedback or bugs
filed as issues.

In the code use it like:

    if (EXPERIMENTAL) {
        ...
    }

in case it does not interfere with other code or does not depend on an `#ifdef`
where it would break default build.

Or:

    #if EXPERIMENTAL
    ...
    #endif

for larger code blocks.

Each feature should be tracked in an issue with label
[experimental](https://github.com/kdave/btrfs-progs/labels/experimental), with
a description and a todo list items. Individual tasks can be tracked in other
issues if needed.
