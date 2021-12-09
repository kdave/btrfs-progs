Auto-repair on read
===================

Data or metadata that are found to be damaged (eg. because the checksum does
not match) at the time they're read from the device can be salvaged in case the
filesystem has another valid copy when using block group profile with redundancy
(DUP, RAID1, RAID5/6). The correct data are returned to the user application
and the damaged copy is replaced by it.
