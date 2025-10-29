Scrub is a validation pass over all filesystem data and metadata that detects
data checksum errors, basic super block errors, basic metadata block header errors,
and disk read errors.

Scrub is done on a per-device base, if a device is specified to :command:`btrfs scrub start`,
then only that device will be scrubbed. Although btrfs will also try to read
other device to find a good copy, if the mirror on that specified device failed
to be read or pass verification.

If a path of btrfs is specified to :command:`btrfs scrub start`, btrfs will scrub
all devices in parallel.

On filesystems that use replicated block group profiles (e.g. RAID1), read-write
scrub will also automatically repair any damage by copying verified good data
from one of the other replicas.

Such automatic repair is also carried out when reading metadata or data from a
read-write mounted filesystem.

.. warning::
   As currently implemented, setting the ``NOCOW`` file attribute (by
   :command:`chattr +C`) on a file implicitly enables
   ``NODATASUM``. This means that while metadata for these files continues to
   be validated and corrected by scrub, the actual file data is not.

   Furthermore, btrfs does not currently mark missing or failed disks as
   unreliable, so will continue to load-balance reads to potentially damaged
   replicas. This is not a problem normally because damage is detected by
   checksum validation, but because ``NOCOW`` files are
   not protected by checksums, btrfs has no idea which mirror is good thus it can
   return the bad contents to the user space tool.

   Detecting and recovering from such failure requires manual intervention.

   Notably, `systemd sets +C on journals by default <https://github.com/systemd/systemd/commit/11689d2a021d95a8447d938180e0962cd9439763>`__,
   and `libvirt ≥ 6.6 sets +C on storage pool directories by default <https://www.libvirt.org/news.html#v6-6-0-2020-08-02>`__.
   Other applications or distributions may also set ``+C`` to try to improve
   performance.

.. note::
   Scrub is not a filesystem checker (fsck, :doc:`btrfs-check`). It can only detect
   filesystem damage using the checksum validation, and it can only repair
   filesystem damage by copying from other known good replicas.

   :doc:`btrfs-check` performs more exhaustive checking and can sometimes be
   used, with expert guidance, to rebuild certain corrupted filesystem structures
   in the absence of any good replica.

.. note::
   Read-only scrub on a read-write filesystem will cause some writes into the
   filesystem.

   This is due to the design limitation to prevent race between marking block
   group read-only and writing back block group items.

   To avoid any writes from scrub, one has to run read-only scrub on read-only
   filesystem.

.. note::
   Scrub can be interrupted by various events after v6.19 kernel, including
   but not limited to power management suspend/hibernate, filesystem freezing,
   cgroup freezing (utilized by systemd for slice freezing) and pending signals.

   The running scrub will be cancelled after such interruption, and can be resumed
   by :command:`btrfs scrub resume` command.

The user is supposed to run it manually or via a periodic system service. The
recommended period is a month but it could be less. The estimated device bandwidth
utilization is about 80% on an idle filesystem.

The scrubbing status is recorded in :file:`/var/lib/btrfs/` in textual files named
*scrub.status.UUID* for a filesystem identified by the given UUID. (Progress
state is communicated through a named pipe in file *scrub.progress.UUID* in the
same directory.) The status file is updated every 5 seconds. A resumed scrub
will continue from the last saved position.

Scrub can be started only on a mounted filesystem, though it's possible to
scrub only a selected device. See :ref:`btrfs scrub start<man-scrub-start>` for more.

.. duplabel:: scrub-io-limiting

Bandwidth and IO limiting
^^^^^^^^^^^^^^^^^^^^^^^^^

.. note::
   The :manref:`ionice(1)` may not be generally supported by all IO schedulers and
   the options to :command:`btrfs scrub start` may not work as expected.

In the past when the `CFQ IO scheduler
<https://en.wikipedia.org/wiki/Completely_fair_queueing>`__ was generally used
the :manref:`ionice(1)` syscalls set the priority to *idle* so the IO would not
interfere with regular IO. Since the kernel 5.0 the CFQ is not available.

The IO scheduler known to support that is `BFQ
<https://docs.kernel.org/block/bfq-iosched.html>`__, but first read the
documentation before using it!

For other commonly used schedulers like `mq-deadline
<https://docs.kernel.org/block/blk-mq.html>`__ it's recommended to use
*cgroup2 IO controller* which could be managed by e.g. *systemd*
(documented in ``systemd.resource-control``). However, starting scrub like that
is not yet completely straightforward. The IO controller must know the physical
device of the filesystem and create a slice so all processes started from that
belong to the same accounting group.

.. code-block:: bash

   $ systemd-run -p "IOReadBandwidthMax=/dev/sdx 10M" btrfs scrub start -B /

Since linux 5.14 it's possible to set the per-device bandwidth limits in a
BTRFS-specific way using files :file:`/sys/fs/btrfs/FSID/devinfo/DEVID/scrub_speed_max`.
This setting is not persistent, lasts until the filesystem is unmounted.
Currently set limits can be displayed by command :ref:`btrfs scrub
limit<man-scrub-limit>`.

.. code-block:: bash

   $ echo 100m > /sys/fs/btrfs/9b5fd16e-1b64-4f9b-904a-74e74c0bbadc/devinfo/1/scrub_speed_max
   $ btrfs scrub limit /
   UUID: 9b5fd16e-1b64-4f9b-904a-74e74c0bbadc
   Id      Limit      Path
   --  ---------  --------
    1  100.00MiB  /dev/sdx
