Reflink
=======

Reflink is a type of shallow copy of file data that shares the blocks but
otherwise the files are independent and any change to the file will not affect
the other. This builds on the underlying COW mechanism. A reflink will
effectively create only a separate metadata pointing to the shared blocks which
is typically much faster than a deep copy of all blocks.

The reflink is typically meant for whole files but a partial file range can be
also copied, though there are no ready-made tools for that.

.. code-block:: shell

   cp --reflink=always source target

There are some constraints:

- cross-filesystem reflink is not possible, there's nothing in common between
  so the block sharing can't work
- reflink crossing two mount points of the same filesystem support depends on
  kernel version:

  - until 5.17 it's not supported and fails with "Cross device link", can be
    worked around by performing the operation on the toplevel subvolume
  - works since 5.18
- reflink requires source and target file that have the same status regarding
  NOCOW and checksums, for example if the source file is NOCOW (once created
  with the chattr +C attribute) then the above command won't work unless the
  target file is pre-created with the +C attribute as well, or the NOCOW
  attribute is inherited from the parent directory (chattr +C on the directory)
  or if the whole filesystem is mounted with *-o nodatacow* that would create
  the NOCOW files by default
