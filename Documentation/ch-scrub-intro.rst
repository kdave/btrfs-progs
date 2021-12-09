Scrub is a pass over all filesystem data and metadata and verifying the
checksums. If a valid copy is available (replicated block group profiles) then
the damaged one is repaired. All copies of the replicated profiles are validated.

.. note::
   Scrub is not a filesystem checker (fsck) and does not verify nor repair
   structural damage in the filesystem. It really only checks checksums of data
   and tree blocks, it doesn't ensure the content of tree blocks is valid and
   consistent. There's some validation performed when metadata blocks are read
   from disk but it's not extensive and cannot substitute full *btrfs check*
   run.

The user is supposed to run it manually or via a periodic system service. The
recommended period is a month but could be less. The estimated device bandwidth
utilization is about 80% on an idle filesystem. The IO priority class is by
default *idle* so background scrub should not significantly interfere with
normal filesystem operation. The IO scheduler set for the device(s) might not
support the priority classes though.

The scrubbing status is recorded in */var/lib/btrfs/* in textual files named
*scrub.status.UUID* for a filesystem identified by the given UUID. (Progress
state is communicated through a named pipe in file *scrub.progress.UUID* in the
same directory.) The status file is updated every 5 seconds. A resumed scrub
will continue from the last saved position.

Scrub can be started only on a mounted filesystem, though it's possible to
scrub only a selected device. See **btrfs scrub start** for more.

