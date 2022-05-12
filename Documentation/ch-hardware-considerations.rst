The following is based on information publicly available, user feedback,
community discussions or bug report analyses. It's not complete and further
research is encouraged when in doubt.

MAIN MEMORY
^^^^^^^^^^^

The data structures and raw data blocks are temporarily stored in computer
memory before they get written to the device. It is critical that memory is
reliable because even simple bit flips can have vast consequences and lead to
damaged structures, not only in the filesystem but in the whole operating
system.

Based on experience in the community, memory bit flips are more common than one
would think. When it happens, it's reported by the tree-checker or by a checksum
mismatch after reading blocks. There are some very obvious instances of bit
flips that happen, e.g. in an ordered sequence of keys in metadata blocks. We can
easily infer from the other data what values get damaged and how. However, fixing
that is not straightforward and would require cross-referencing data from the
entire filesystem to see the scope.

If available, ECC memory should lower the chances of bit flips, but this
type of memory is not available in all cases. A memory test should be performed
in case there's a visible bit flip pattern, though this may not detect a faulty
memory module because the actual load of the system could be the factor making
the problems appear. In recent years attacks on how the memory modules operate
have been demonstrated ('rowhammer') achieving specific bits to be flipped.
While these were targeted, this shows that a series of reads or writes can
affect unrelated parts of memory.

Further reading:

* https://en.wikipedia.org/wiki/Row_hammer

What to do:

* run *memtest*, note that sometimes memory errors happen only when the system
  is under heavy load that the default memtest cannot trigger
* memory errors may appear as filesystem going read-only due to "pre write"
  check, that verify meta data before they get written but fail some basic
  consistency checks

DIRECT MEMORY ACCESS (DMA)
^^^^^^^^^^^^^^^^^^^^^^^^^^

Another class of errors is related to DMA (direct memory access) performed
by device drivers. While this could be considered a software error, the
data transfers that happen without CPU assistance may accidentally corrupt
other pages. Storage devices utilize DMA for performance reasons, the
filesystem structures and data pages are passed back and forth, making
errors possible in case page life time is not properly tracked.

There are lots of quirks (device-specific workarounds) in Linux kernel
drivers (regarding not only DMA) that are added when found. The quirks
may avoid specific errors or disable some features to avoid worse problems.

What to do:

* use up-to-date kernel (recent releases or maintained long term support versions)
* as this may be caused by faulty drivers, keep the systems up-to-date

ROTATIONAL DISKS (HDD)
^^^^^^^^^^^^^^^^^^^^^^

Rotational HDDs typically fail at the level of individual sectors or small clusters.
Read failures are caught on the levels below the filesystem and are returned to
the user as *EIO - Input/output error*. Reading the blocks repeatedly may
return the data eventually, but this is better done by specialized tools and
filesystem takes the result of the lower layers. Rewriting the sectors may
trigger internal remapping but this inevitably leads to data loss.

Disk firmware is technically software but from the filesystem perspective is
part of the hardware. IO requests are processed, and caching or various
other optimizations are performed, which may lead to bugs under high load or
unexpected physical conditions or unsupported use cases.

Disks are connected by cables with two ends, both of which can cause problems
when not attached properly. Data transfers are protected by checksums and the
lower layers try hard to transfer the data correctly or not at all. The errors
from badly-connecting cables may manifest as large amount of failed read or
write requests, or as short error bursts depending on physical conditions.

What to do:

* check **smartctl** for potential issues

SOLID STATE DRIVES (SSD)
^^^^^^^^^^^^^^^^^^^^^^^^

The mechanism of information storage is different from HDDs and this affects
the failure mode as well. The data are stored in cells grouped in large blocks
with limited number of resets and other write constraints. The firmware tries
to avoid unnecessary resets and performs optimizations to maximize the storage
media lifetime. The known techniques are deduplication (blocks with same
fingerprint/hash are mapped to same physical block), compression or internal
remapping and garbage collection of used memory cells. Due to the additional
processing there are measures to verity the data e.g. by ECC codes.

The observations of failing SSDs show that the whole electronic fails at once
or affects a lot of data (eg. stored on one chip). Recovering such data
may need specialized equipment and reading data repeatedly does not help as
it's possible with HDDs.

There are several technologies of the memory cells with different
characteristics and price. The lifetime is directly affected by the type and
frequency of data written.  Writing "too much" distinct data (e.g. encrypted)
may render the internal deduplication ineffective and lead to a lot of rewrites
and increased wear of the memory cells.

There are several technologies and manufacturers so it's hard to describe them
but there are some that exhibit similar behaviour:

* expensive SSD will use more durable memory cells and is optimized for
  reliability and high load
* cheap SSD is projected for a lower load ("desktop user") and is optimized for
  cost, it may employ the optimizations and/or extended error reporting
  partially or not at all

It's not possible to reliably determine the expected lifetime of an SSD due to
lack of information about how it works or due to lack of reliable stats provided
by the device.

Metadata writes tend to be the biggest component of lifetime writes to a SSD,
so there is some value in reducing them. Depending on the device class (high
end/low end) the features like DUP block group profiles may affect the
reliability in both ways:

* *high end* are typically more reliable and using 'single' for data and
  metadata could be suitable to reduce device wear
* *low end* could lack ability to identify errors so an additional redundancy
  at the filesystem level (checksums, *DUP*) could help

Only users who consume 50 to 100% of the SSD's actual lifetime writes need to be
concerned by the write amplification of btrfs DUP metadata. Most users will be
far below 50% of the actual lifetime, or will write the drive to death and
discover how many writes 100% of the actual lifetime was. SSD firmware often
adds its own write multipliers that can be arbitrary and unpredictable and
dependent on application behavior, and these will typically have far greater
effect on SSD lifespan than DUP metadata. It's more or less impossible to
predict when a SSD will run out of lifetime writes to within a factor of two, so
it's hard to justify wear reduction as a benefit.

Further reading:

* https://www.snia.org/educational-library/ssd-and-deduplication-end-spinning-disk-2012
* https://www.snia.org/educational-library/realities-solid-state-storage-2013-2013
* https://www.snia.org/educational-library/ssd-performance-primer-2013
* https://www.snia.org/educational-library/how-controllers-maximize-ssd-life-2013

What to do:

* run **smartctl** or self-tests to look for potential issues
* keep the firmware up-to-date

NVM EXPRESS, NON-VOLATILE MEMORY (NVMe)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

NVMe is a type of persistent memory usually connected over a system bus (PCIe)
or similar interface and the speeds are an order of magnitude faster than SSD.
It is also a non-rotating type of storage, and is not typically connected by a
cable. It's not a SCSI type device either but rather a complete specification
for logical device interface.

In a way the errors could be compared to a combination of SSD class and regular
memory. Errors may exhibit as random bit flips or IO failures. There are tools
to access the internal log (**nvme log** and **nvme-cli**) for a more detailed
analysis.

There are separate error detection and correction steps performed e.g. on the
bus level and in most cases never making in to the filesystem level. Once this
happens it could mean there's some systematic error like overheating or bad
physical connection of the device. You may want to run self-tests (using
**smartctl**).

* https://en.wikipedia.org/wiki/NVM_Express
* https://www.smartmontools.org/wiki/NVMe_Support

DRIVE FIRMWARE
^^^^^^^^^^^^^^

Firmware is technically still software but embedded into the hardware. As all
software has bugs, so does firmware. Storage devices can update the firmware
and fix known bugs. In some cases the it's possible to avoid certain bugs by
quirks (device-specific workarounds) in Linux kernel.

A faulty firmware can cause wide range of corruptions from small and localized
to large affecting lots of data. Self-repair capabilities may not be sufficient.

What to do:

* check for firmware updates in case there are known problems, note that
  updating firmware can be risky on itself
* use up-to-date kernel (recent releases or maintained long term support versions)

SD FLASH CARDS
^^^^^^^^^^^^^^

There are a lot of devices with low power consumption and thus using storage
media based on low power consumption too, typically flash memory stored on
a chip enclosed in a detachable card package. An improperly inserted card may be
damaged by electrical spikes when the device is turned on or off. The chips
storing data in turn may be damaged permanently. All types of flash memory
have a limited number of rewrites, so the data are internally translated by FTL
(flash translation layer). This is implemented in firmware (technically a
software) and prone to bugs that manifest as hardware errors.

Adding redundancy like using DUP profiles for both data and metadata can help
in some cases but a full backup might be the best option once problems appear
and replacing the card could be required as well.

HARDWARE AS THE MAIN SOURCE OF FILESYSTEM CORRUPTIONS
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**If you use unreliable hardware and don't know about that, don't blame the
filesystem when it tells you.**
