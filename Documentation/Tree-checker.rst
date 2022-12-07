Tree checker
============

Tree checker is a feature that verifies metadata blocks before write or after
read from the devices.  The b-tree nodes contain several items describing the
filesystem structures and to some degree can be verified for consistency or
validity. This is an additional check to the checksums that only verify the
overall block status while the tree checker tries to validate and cross
reference the logical structure. This takes a slight performance hit but is
comparable to calculating the checksum and has no noticeable impact while it
does catch all sorts of errors.

There are two occasions when the checks are done:

Pre-write checks
----------------

When metadata blocks are in memory and about to be written to the permanent
storage, the checks are performed, before the checksums are calculated. This
can catch random corruptions of the blocks (or pages) either caused by bugs or
by other parts of the system or hardware errors (namely faulty RAM).

Once a block does not pass the checks, the filesystem refuses to write more data
and turns itself to read-only mode to prevent further damage. At this point some
the recent metadata updates are held *only* in memory so it's best to not panic
and try to remember what files could be affected and copy them elsewhere. Once
the filesystem gets unmounted, the most recent changes are unfortunately lost.
The filesystem that is stored on the device is still consistent and should mount
fine.

A message may look like:

.. code-block::

   [ 1716.823895] BTRFS critical (device vdb): corrupt leaf: root=18446744073709551607 block=38092800 slot=0, invalid key objectid: has 1 expect 6 or [256, 18446744073709551360] or 18446744073709551604
   [ 1716.829499] BTRFS info (device vdb): leaf 38092800 gen 19 total ptrs 4 free space 15851 owner 18446744073709551607
   [ 1716.832891] BTRFS info (device vdb): refs 3 lock (w:0 r:0 bw:0 br:0 sw:0 sr:0) lock_owner 0 current 1506
   [ 1716.836054]  item 0 key (1 1 0) itemoff 16123 itemsize 160
   [ 1716.837993]          inode generation 1 size 0 mode 100600
   [ 1716.839760]  item 1 key (256 1 0) itemoff 15963 itemsize 160
   [ 1716.841742]          inode generation 4 size 0 mode 40755
   [ 1716.843393]  item 2 key (256 12 256) itemoff 15951 itemsize 12
   [ 1716.845320]  item 3 key (18446744073709551611 48 1) itemoff 15951 itemsize 0
   [ 1716.847505] BTRFS error (device vdb): block=38092800 write time tree block corruption detected

The line(s) before the *write time tree block corruption detected* message is
specific to the found error.

Post-read checks
----------------

Metadata blocks get verified right after they're read from devices and the
checksum is found to be valid. This protects against changes to the metadata
that could possibly also update the checksum, less likely to happen accidentally
but rather due to intentional corruption or fuzzing.

.. code-block::

   [ 4823.612832] BTRFS critical (device vdb): corrupt leaf: root=7 block=30474240 slot=0, invalid nritems, have 0 should not be 0 for non-root leaf
   [ 4823.616798] BTRFS error (device vdb): block=30474240 read time tree block corruption detected

The checks
----------

As implemented right now, the metadata consistency is limited to one b-tree node
and what items are stored there, i.e. there's no extensive or broad check done
e.g. against other data structures in other b-tree nodes. This still provides
enough opportunities to verify consistency of individual items, besides verifying
general validity of the items like the length or offset. The b-tree items are
also coupled with a key so proper key ordering is also part of the check and can
reveal random bitflips in the sequence (this has been the most successful
detector of faulty RAM).

The capabilities of tree checker have been improved over time and it's possible
that a filesystem created on an older kernel may trigger warnings or fail some
checks on a new one.

Reporting problems
------------------

In many cases the bug is caused by hardware and cannot be automatically fixed
by *btrfs check --repair*, so do not try that without being advised to. Even if
the error is unfixable it's useful to report it, either to validate the cause
but also to give more ideas how to improve the tree checker.  Please consider
reporting it to the mailing list *linux-btrfs@vger.kernel.org*.
