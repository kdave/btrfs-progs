Scrub is a validation pass over all filesystem data and metadata that detects
checksum errors, super block errors, metadata block header errors, and disk
read errors. All copies of replicated profiles are validated by default.

On filesystems that use replicated block group profiles (e.g. raid1), scrub will
also automatically repair any damage by default by copying verified good data
from one of the other replicas.

.. warning::
   Setting the ``No_COW`` (``chattr +C``) attribute on a file implicitly enables
   ``nodatasum``. This means that while metadata for these files continues to
   be validated and corrected by scrub, the actual file data is not.

   Furthermore, btrfs does not currently mark missing or failed disks as
   unreliable, so will continue to load-balance reads to potentially damaged
   replicas in a replicated filesystem. This is not a problem normally because
   damage is detected by checksum validation and a mirror copy is used, but
   because ``No_COW`` files are not protected by checksum, bad data may be
   returned even if a good copy exists on another replica. Which replica is used
   is determined by the setting in ``/sys/fs/btrfs/<uuid>/read_policy``.
   Currently, the only possible value for this setting is ``pid``, which uses
   the process ID of the executable reading the file to pick the replica.

   Writing to a ``No_COW`` file after reading from a bad replica will overwrite
   all replicas with the bad data. Detecting and recovering from a failure in
   this case requires manual intervention before the file is rewritten to avoid
   data loss. See issue `#482 <https://github.com/kdave/btrfs-progs/issues/482>`_.
   Even with raid1c3 or higher, for performance reasons, btrfs does not use
   consensus reads on any files, even ``No_COW`` files, to validate or correct
   data errors.

   Notably, `systemd sets +C on journals by default <https://github.com/systemd/systemd/commit/11689d2a021d95a8447d938180e0962cd9439763>`_,
   and `libvirt ≥ 6.6 sets +C on storage pool directories by default <https://www.libvirt.org/news.html#v6-6-0-2020-08-02>`_.
   Other applications or distributions may also set +C to try to improve
   performance.

.. warning::
   A read-write scrub will do no further harm to a damaged filesystem if it is not
   possible to perform a correct repair, so it is safe to use at almost any time.
   However, if a split-brain event occurs, btrfs scrub may cause unrecoverable data
   loss. This situation is unlikely and requires a specific sequence of events that
   cause an unhealthy device or device set to be mounted read-write in the absence
   of the healthy device or device set from the same filesystem. For example:

   1. Device set F fails and drops from the bus, while device set H continues to
      function and receive additional writes.
   2. After a reboot, healthy set H does not reappear immediately, but failed set
      F does.
   3. Failed set F is mounted read-write. At this point, it is no longer safe for
      set H to reappear as the transaction histories have diverged. Allowing set H
      and set F to recombine at any point will cause corruption of set H. Running
      scrub on a split-brained filesystem will overwrite good data from set H with
      other data from set F, increasing the amount of permanent data loss.

.. note::
   Scrub is not a filesystem checker (fsck). It can only detect filesystem damage
   using the (:doc:`Tree-checker`) and checksum validation, and it can only repair
   filesystem damage by copying from other known good replicas.

   :doc:`btrfs-check` performs more exhaustive checking and can sometimes be
   used, with expert guidance, to rebuild certain corrupted filesystem structures
   in the absence of any good replica. However, when a replica exists, scrub is
   able to automatically correct most errors reported by ``btrfs-check``, so should
   normally be run first to avoid false positives from ``btrfs-check``.

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
