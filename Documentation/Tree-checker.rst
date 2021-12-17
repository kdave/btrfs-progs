Tree checker
============

Metadata blocks that have been just read from devices or are just about to be
written are verified and sanity checked by so called **tree checker**. The
b-tree nodes contain several items describing the filesystem structure and to
some degree can be verified for consistency or validity. This is additional
check to the checksums that only verify the overall block status while the tree
checker tries to validate and cross reference the logical structure. This takes
a slight performance hit but is comparable to calculating the checksum and has
no noticeable impact while it does catch all sorts of errors.

There are two occasions when the checks are done:

Pre-write checks
----------------

When metadata blocks are in memory about to be written to the permanent storage,
the checks are performed, before the checksums are calculated. This can catch
random corruptions of the blocks (or pages) either caused by bugs or by other
parts of the system or hardware errors (namely faulty RAM).

Once a block does not pass the checks, the filesystem refuses to write more data
and turns itself to read-only mode to prevent further damage. At this point some
the recent metadata updates are held *only* in memory so it's best to not panic
and try to remember what files could be affected and copy them elsewhere. Once
the filesystem gets unmounted, the most recent changes are unfortunately lost.
The filesystem that is stored on the device is still consistent and should mount
fine.

Post-read checks
----------------

Metadata blocks get verified right after they're read from devices and the
checksum is found to be valid. This protects against changes to the metadata
that could possibly also update the checksum, less likely to happen accidentally
but rather due to intentional corruption or fuzzing.

The checks
----------

As implemented right now, the metadata consistency is limited to one b-tree node
and what items are stored there, ie. there's no extensive or broad check done
eg. against other data structures in other b-tree nodes. This still provides
enough opportunities to verify consistency of individual items, besides verifying
general validity of the items like the length or offset. The b-tree items are
also coupled with a key so proper key ordering is also part of the check and can
reveal random bitflips in the sequence (this has been the most successful
detector of faulty RAM).

The capabilities of tree checker have been improved over time and it's possible
that a filesystem created on an older kernel may trigger warnings or fail some
checks on a new one.
