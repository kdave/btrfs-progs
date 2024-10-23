Auto-repair on read
===================

If data or metadata that are found to be damaged at the time they’re read from a device,
for example because the checksum does not match, they can be salvaged if the filesystem
has another valid copy. This can be achieved by using a block group profile with redundancy
like DUP, RAID1-like, or RAID5/6.

The correct data is returned to the user application and the damaged copy is replaced by it.
When this happens, a message is emitted to the system log.

If there are multiple copies of data and one of them is damaged but not read by the user
application, then this is not detected.

To ensure the verification and automatic repair of all data and metadata copies, the
:doc:`scrub<Scrub>` operation must be initiated manually.
