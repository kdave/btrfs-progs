btrfs-send(8)
=============

SYNOPSIS
--------

**btrfs send** [-ve] [-p <parent>] [-c <clone-src>] [-f <outfile>] <subvol> [<subvol>...]

DESCRIPTION
-----------

This command will generate a stream of instructions that describe changes
between two subvolume snapshots. The stream can be consumed by the **btrfs
receive** command to replicate the sent snapshot on a different filesystem.
The command operates in two modes: full and incremental.

All snapshots involved in one send command must be read-only, and this status
cannot be changed as long as there's a running send operation that uses the
snapshot.

In the full mode, the entire snapshot data and metadata will end up in the
stream.

In the incremental mode (options *-p* and *-c*), previously sent snapshots that
are available on both the sending and receiving side can be used to reduce the
amount of information that has to be sent to reconstruct the sent snapshot on a
different filesystem.

The *-p <parent>* option can be omitted when *-c <clone-src>* options are
given, in which case **btrfs send** will determine a suitable parent from among
the clone sources.

You must not specify clone sources unless you guarantee that these snapshots
are exactly in the same state on both sides--both for the sender and the
receiver. For implications of changed read-write status of a received snapshot
please see section *SUBVOLUME FLAGS* in ``btrfs-subvolume(8)``.

``Options``

-e
        if sending multiple subvolumes at once, use the new format and omit the
        'end cmd' marker in the stream separating the subvolumes

-p <parent>
        send an incremental stream from *parent* to *subvol*

-c <clone-src>
        use this snapshot as a clone source for an incremental send (multiple
        allowed)

-f <outfile>
        output is normally written to standard output so it can be, for
        example, piped to btrfs receive. Use this option to write it to a file
        instead.

--no-data::
        send in *NO_FILE_DATA* mode

        The output stream does not contain any file data and thus cannot be
        used to transfer changes. This mode is faster and is useful to show the
        differences in metadata.

-q|--quiet
        (deprecated) alias for global *-q* option

-v|--verbose
        (deprecated) alias for global *-v* option

``Global options``

-q|--quiet
        suppress all messages except errors

-v|--verbose
        increase output verbosity, print generated commands in a readable form

EXIT STATUS
-----------

**btrfs send** returns a zero exit status if it succeeds. Non zero is
returned in case of failure.

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.
Please refer to the btrfs wiki http://btrfs.wiki.kernel.org for
further details.

SEE ALSO
--------

``mkfs.btrfs(8)``,
``btrfs-receive(8)``,
``btrfs-subvolume(8)``
