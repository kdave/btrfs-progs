Scrub is a pass over all filesystem data and metadata and verifying the
checksums. If a valid copy is available (replicated block group profiles) then
the damaged one is repaired. All copies of the replicated profiles are validated.

.. note::
   Scrub is not a filesystem checker (fsck) and does not verify nor repair
   structural damage in the filesystem. It really only checks checksums of data
   and tree blocks, it doesn't ensure the content of tree blocks is valid and
   consistent. There's some validation performed when metadata blocks are read
   from disk (:doc:`Tree-checker`) but it's not extensive and cannot substitute
   full :doc:`btrfs-check` run.

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
