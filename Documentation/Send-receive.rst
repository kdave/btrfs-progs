Send/receive
============

Send and receive are complementary features that allow to transfer data from
one filesystem to another in a streamable format. The send part traverses a
given read-only subvolume and either creates a full stream representation of
its data and metadata (*full mode*), or given a set of subvolumes for reference
it generates a difference relative to that set (*incremental mode*).

Receive on the other hand takes the stream and reconstructs a subvolume with
files and directories equivalent to the filesystem that was used to produce the
stream. The result is not exactly 1:1, e.g. inode numbers can be different and
other unique identifiers can be different (like the subvolume UUIDs). The full
mode starts with an empty subvolume, creates all the files and then turns the
subvolume to read-only. At this point it could be used as a starting point for a
future incremental send stream, provided it would be generated from the same
source subvolume on the other filesystem.

The stream is a sequence of encoded commands that change e.g. file metadata
(owner, permissions, extended attributes), data extents (create, clone,
truncate), whole file operations (rename, delete). The stream can be sent over
network, piped directly to the receive command or saved to a file. Each command
in the stream is protected by a CRC32C checksum.
