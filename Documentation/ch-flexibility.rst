The underlying design of BTRFS data structures allows a lot of flexibility and
making changes after filesystem creation, like resizing, adding/removing space
or enabling some features on-the-fly.

* **dynamic inode creation** -- there's no fixed space or tables for tracking
  inodes so the number of inodes that can be created is bounded by the metadata
  space and its utilization

* **block group profile change on-the-fly** -- the block group profiles can be
  changed on a mounted filesystem by running the balance operation and
  specifying the conversion filters (see :doc:`balance<Balance>`)

* **resize** -- the space occupied by the filesystem on each device can be
  resized up (grow) or down (shrink) as long as the amount of data can be still
  contained on the device

* **device management** -- devices can be added, removed or replaced without
  requiring recreating the filesystem (mkfs), new redundancy options available
  on more devices can be also utilized by rebalancing
