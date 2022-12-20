Changes (kernel/version)
========================

Summary of kernel changes for each version.

6.0 (incomplete)
----------------

* Send protocol version 2

6.1 (incomplete)
----------------

* scrub: fix superblock errors immediately and don't leave it up to the next commit
* send: experimental support for fs-verity (send v3)
* sysfs: export discards stats and tunables
* sysfs: export qgroup global information about status
* sysfs: allow to skip quota recalculation for subvolumes ('drop_subtree_threshold')
* nowait mode for async writes
* check super block after filesystem thaw (detect accidental changes from outside)
