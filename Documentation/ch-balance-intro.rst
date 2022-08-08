The primary purpose of the balance feature is to spread block groups across
all devices so they match constraints defined by the respective profiles. See
:doc:`mkfs.btrfs(8)<mkfs.btrfs>` section *PROFILES* for more details.
The scope of the balancing process can be further tuned by use of filters that
can select the block groups to process. Balance works only on a mounted
filesystem.  Extent sharing is preserved and reflinks are not broken.
Files are not defragmented nor recompressed, file extents are preserved
but the physical location on devices will change.

The balance operation is cancellable by the user. The on-disk state of the
filesystem is always consistent so an unexpected interruption (eg. system crash,
reboot) does not corrupt the filesystem. The progress of the balance operation
is temporarily stored as an internal state and will be resumed upon mount,
unless the mount option *skip_balance* is specified.

.. warning::
   Running balance without filters will take a lot of time as it basically move
   data/metadata from the whole filesystem and needs to update all block
   pointers.

The filters can be used to perform following actions:

- convert block group profiles (filter *convert*)
- make block group usage more compact  (filter *usage*)
- perform actions only on a given device (filters *devid*, *drange*)

The filters can be applied to a combination of block group types (data,
metadata, system). Note that changing only the *system* type needs the force
option. Otherwise *system* gets automatically converted whenever *metadata*
profile is converted.

When metadata redundancy is reduced (eg. from RAID1 to single) the force option
is also required and it is noted in system log.

.. note::
   The balance operation needs enough work space, ie. space that is completely
   unused in the filesystem, otherwise this may lead to ENOSPC reports.  See
   the section *ENOSPC* for more details.

Compatibility
-------------

.. note::

   The balance subcommand also exists under the **btrfs filesystem** namespace.
   This still works for backward compatibility but is deprecated and should not
   be used any more.

.. note::
   A short syntax **btrfs balance <path>** works due to backward compatibility
   but is deprecated and should not be used any more. Use **btrfs balance start**
   command instead.

Performance implications
------------------------

Balancing operations are very IO intensive and can also be quite CPU intensive,
impacting other ongoing filesystem operations. Typically large amounts of data
are copied from one location to another, with corresponding metadata updates.

Depending upon the block group layout, it can also be seek heavy. Performance
on rotational devices is noticeably worse compared to SSDs or fast arrays.
