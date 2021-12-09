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
