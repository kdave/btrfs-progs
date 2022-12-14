Auto-repair on read
===================

Data or metadata that are found to be damaged (e.g. because the checksum does
not match) at the time they're read from a device can be salvaged in case the
filesystem has another valid copy when using block group profile with redundancy
(DUP, RAID1-like, RAID5/6). The correct data are returned to the user application
and the damaged copy is replaced by it. When this happen a message is emitted
to the system log.

If there are more copies of data and one of them is damaged but not read by
user application then this is not detected. To verify all data and metadata
copies there's :doc:`scrub<Scrub>` that needs to be started manually, automatic
repairs happens in that case.
