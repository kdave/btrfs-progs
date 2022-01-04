Inline files
============

Files up to some size can be stored in the metadata section ("inline" in the
b-tree nodes), ie. no separate blocks for the extents. The default limit is
2048 bytes and can be configured by mount option ``max_inline``.  The data of
inlined files can be also compressed as long as they fit into the b-tree nodes.

If the filesystem has been created with different data and metadata profiles,
namely with different level of integrity, this also affects the inlined files.
It can be completely disabled by mounting with ``max_inline=0``. The upper
limit is either the size of b-tree node or the page size of the host.

An inline file can be identified by enumerating the extents, eg. by the tool
``filefrag``:

.. code-block:: bash

   $ filefrag -v inlinefile
   Filesystem type is: 9123683e
   File size of inlinefile is 463 (1 block of 4096 bytes)
    ext:     logical_offset:        physical_offset: length:   expected: flags:
      0:        0..    4095:          0..      4095:   4096:             last,not_aligned,inline,eof

In the above example, the file is not compressed, otherwise it would have the
*encoded* flag. The inline files have no limitations and behave like regular
files with respect to copying, renaming, reflink, truncate etc.
