Defragmentation
===============

Defragmentation of files is supposed to make the layout of the file extents to
be more linear or at least coalesce the file extents into larger ones that can
be stored on the device more efficiently. The reason there's a need for
defragmentation stems from the COW design that BTRFS is built on and is
inherent. The fragmentation is caused by rewrites of the same file data
in-place, that has to be handled by creating a new copy that may lie on a
distant location on the physical device. Fragmentation is the worst problem on
rotational hard disks due to the delay caused by moving the drive heads to the
distant location. With the modern seek-less devices it's not a problem though
it may still make sense because of reduced size of the metadata that's needed
to track the scattered extents.

File data that are in use can be safely defragmented because the whole process
happens inside the page cache, that is the central point caching the file data
and takes care of synchronization. Once a filesystem sync or flush is started
(either manually or automatically) all the dirty data get written to the
devices. This however reduces the chances to find optimal layout as the writes
happen together with other data and the result depends on the remaining free
space layout and fragmentation.

.. warning::
   Defragmentation does not preserve extent sharing, e.g. files created by **cp
   --reflink** or existing on multiple snapshots. Due to that the data space
   consumption may increase.

Defragmentation can be started together with compression on the given range,
and takes precedence over per-file compression property or mount options.
