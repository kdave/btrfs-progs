Terms in *italics* also appear in this glossary.

allocator
	Usually *allocator* means the *block* allocator, i.e. the logic
	inside the filesystem which decides where to place newly allocated blocks
	in order to maintain several constraints (like data locality, low
	fragmentation).

	In btrfs, allocator may also refer to *chunk* allocator, i.e. the
	logic behind placing chunks on devices.

balance
	An operation that can be done to a btrfs filesystem, for example
	through :command:`btrfs balance /path`. A
	balance passes all data in the filesystem through the *allocator*
	again. It is primarily intended to rebalance the data in the filesystem
	across the *devices* when a device is added or removed. A balance
	will regenerate missing copies for the redundant *RAID* levels, if a
	device has failed. As of Linux kernel 3.3, a balance operation can be
	made selective about which parts of the filesystem are rewritten
        using :ref:`filters<man-balance-filters>`.

barrier
	An instruction to the underlying hardware to ensure that everything before
	the barrier is physically written to permanent storage before anything
	after it. Used in btrfs's *copy on write* approach to ensure
	filesystem consistency.

block
	A single physically and logically contiguous piece of storage on a
        device, of size e.g. 4K. In some contexts also referred to as *sector*,
        though the term *block* is preferred.

block group
	The unit of allocation of space in btrfs. A block group is laid out on
	the disk by the btrfs *allocator*, and will consist of one or more
	*chunks*, each stored on a different *device*. The number of chunks
	used in a block group will depend on its *RAID* level.

B-tree
	The fundamental storage data structure used in btrfs. Except for the
	*superblocks*, all of btrfs *metadata* is stored in one of several
	B-trees on disk. B-trees store key/item pairs. While the same code is
	used to implement all of the B-trees, there are a few different
        categories of B-tree. The name *btrfs* refers to its use of B-trees.

btrfsck, fsck, btrfs-check
	Tool in *btrfs-progs* that checks an unmounted filesystem (*offline*)
        and reports on any errors in the filesystem structures it finds.  By
        default the tool runs in read-only mode as fixing errors is potentially
        dangerous.  See also *scrub*.

btrfs-progs
	User mode tools to manage btrfs-specific features. Maintained at
        http://github.com/kdave/btrfs-progs.git . The main frontend to btrfs
        features is the standalone tool *btrfs*, although
        other tools such as *mkfs.btrfs* and *btrfstune* are also part of
        btrfs-progs.

chunk
	A part of a *block group*. Chunks are either 1 GiB in size (for data)
	or 256 MiB (for *metadata*), depending on the overall filesystem size.

chunk tree
	A layer that keeps information about mapping between physical and
	logical block addresses. It's stored within the *system* group.

cleaner
	Usually referred to in context of deleted subvolumes. It's a background
	process that removes the actual data once a subvolume has been deleted.
	Cleaning can involve lots of IO and CPU activity depending on the
	fragmentation and amount of shared data with other subvolumes.

        The cleaner kernel thread also processes defragmentation triggered by the
        *autodefrag* mount option, removing of empty blocks groups and some
        other finalization tasks.

copy-on-write, COW
	Also known as *COW*. The method that btrfs uses for modifying data.
	Instead of directly overwriting data in place, btrfs takes a copy of
	the data, alters it, and then writes the modified data back to a
	different (unused) location on the disk. It then updates the *metadata*
	to reflect the new location of the data. In order to update the
	metadata, the affected metadata blocks are also treated in the same
	way. In COW filesystems, files tend to fragment as they are modified.
	Copy-on-write is also used in the implementation of *snapshots* and
	*reflink copies*. A copy-on-write filesystem is, in theory,
	*always* consistent, provided the underlying hardware supports
	*barriers*.

default subvolume
	The *subvolume* in a btrfs filesystem which is mounted when mounting
	the filesystem without using the ``subvol=`` mount option.

device
	A Linux block device, e.g. a whole disk, partition, LVM logical volume,
	loopback device, or network block device. A btrfs filesystem can reside
	on one or more devices.

df
	A standard Unix tool for reporting the amount of space used and free in
	a filesystem. The standard tool does not give accurate results, but the
	*btrfs* command from *btrfs-progs* has
	an implementation of *df* which shows space available in more detail. See
	the
	[[FAQ#Why_does_df_show_incorrect_free_space_for_my_RAID_volume.3F|FAQ]]
	for a more detailed explanation of btrfs free space accounting.

DUP
	A form of "*RAID*" which stores two copies of each piece of data on
	the same *device*. This is similar to *RAID1*, and protects
	against *block*-level errors on the device, but does not provide any
	guarantees if the entire device fails. By default, btrfs uses *DUP*
	profile for metadata on single device filesystem.s

ENOSPC
	Error code returned by the OS to a user program when the filesystem
	cannot allocate enough data to fulfill the user request. In most
	filesystems, it indicates there is no free space available in the
	filesystem. Due to the additional space requirements from btrfs's
	*COW* behaviour, btrfs can sometimes return ENOSPC when there is
	apparently (in terms of *df*) a large amount of space free. This is
	effectively a bug in btrfs, and (if it is repeatable), using the mount
	option ``enospc_debug`` may give a report
	that will help the btrfs developers. See the
	[[FAQ#if_your_device_is_large_.28.3E16GiB.29|FAQ entry]] on free space.

extent
	Contiguous sequence of bytes on disk that holds file data. It's a compact
        representation that tracks the start and length of the byte range, so the
        logic behind allocating blocks (*delayed allocation*) strives for
        maximizing the length before writing the extents to the devices.

extent buffer
        An abstraction of a *b-tree* metadata block storing item keys and item
        data. The underlying related structures are physical device block and a
        CPU memory page.

fallocate
	Command line tool in util-linux, and a syscall, that reserves space in
	the filesystem for a file, without actually writing any file data to
	the filesystem. First data write will turn the preallocated extents
        into regular ones. See :manref:`fallocate(1)` and :manref:`fallocate(2)` manual pages
        for more details.

filefrag
	A tool to show the number of extents in a file, and hence the amount of
	fragmentation in the file. It is usually part of the e2fsprogs package
	on most Linux distributions. While initially developed for the ext2
	filesystem, it works on Btrfs as well. It uses the *FIEMAP* ioctl.

free space cache
        Also known as "space cache v1". A separate cache tracking free space as
        btrfs only tracks the allocated space. The free space is by definition
        any hole between allocated ranges. Finding the free ranges can be
        I/O intensive so the cache stores a condensed representation of it.
        It is updated every *transaction* commit.

        The v1 free space cache has been superseded by free space tree.

free space tree
        Successor of *free space cache*, also known as "space cache v2" and now
        default. The free space is tracked in a better way and using COW
        unlike a custom mechanism of v1.

fsync
	On Unix and Unix-like operating systems (of which Linux is the latter),
	the :manref:`fsync(2)` system call causes all buffered file
	descriptor related data changes to be flushed to the underlying block
	device. When a file is modified on a modern operating system the
	changes are generally not written to the disk immediately but rather
        those changes are buffered in memory for performance reasons,
        calling :manref:`fsync(2)` causes any in-memory changes to be written
	to disk.

generation
	An internal counter which updates for each *transaction*. When a
	*metadata* block is written (using *copy on write*), current
	generation is stored in the block, so that blocks which are too new
	(and hence possibly inconsistent) can be identified.

key
	A fixed sized tuple used to identify and sort items in a *B-tree*.
	The key is broken up into 3 parts: *objectid*, *type*, and
	*offset*. The *type* field indicates how each of the other two
	fields should be used, and what to expect to find in the item.

item
	A variable sized structure stored in B-tree leaves. Items hold
	different types of data depending on key type.

log tree
        A b-tree that temporarily tracks ongoing metadata updates until a full
        transaction commit is done. It's a performance optimization of
        *fsync*. The log tracked in the tree are replayed if the filesystem
        is not unmounted cleanly.

metadata
	Data about data. In btrfs, this includes all of the internal data
	structures of the filesystem, including directory structures,
	filenames, file permissions, checksums, and the location of each file's
	*extents*. All btrfs metadata is stored in *B-trees*.

mkfs.btrfs
	The tool (from *btrfs-progs*) to create a btrfs filesystem.

offline
	A filesystem which is not mounted is offline. Some tools (e.g.
	*btrfsck*) will only work on offline filesystems. Compare *online*.

online
	A filesystem which is mounted is online. Most btrfs tools will only
	work on online filesystems. Compare *offline*.

orphan
        A file that's still in use (opened by a running process) but all
        directory entries of that file have been removed.

RAID
	A class of different methods for writing some additional redundant data
	across multiple *devices* so that if one device fails, the missing
	data can be reconstructed from the remaining ones. See *RAID0*,
	*RAID1*, *RAID5*, *RAID6*, *RAID10*, *DUP* and
	*single*. Traditional RAID methods operate across multiple devices of
	equal size, whereas btrfs' RAID implementation works inside *block
	groups*.

RAID0
	A form of *RAID* which provides no guarantees of error recovery, but
	stripes a single copy of data across multiple devices for performance
	purposes. The stripe size is fixed to 64KB for now.

RAID1, RAID1C3, RAID1C4
        A form of *RAID* which stores two/three/four complete copies of each
        piece of data. Each copy is stored on a different *device*. btrfs
        requires a minimum of two devices to use RAID-1 or three/four respectively.
        This is the default block group profile for btrfs's *metadata* on more
        than one device.

RAID5
	A form of *RAID* which stripes a single copy of data across multiple
	*devices*, including one device's worth of additional parity data.
	Can be used to recover from a single device failure.

RAID6
	A form of *RAID* which stripes a single copy of data across multiple
	*devices*, including two device's worth of additional parity data. Can
	be used to recover from the failure of two devices.

RAID10
	A form of *RAID* which stores two complete copies of each piece of
	data, and also stripes each copy across multiple devices for
	performance.

reflink
        Commonly used as a reference to a shallow copy of file extents that share
        the extents until the first change. Reflinked files (e.g. by the :command:`cp`)
        are different files but point to the same extents, any change will be
        detected and new copy of the data created, keeping the files independent.
        Related to that is extent range cloning, that works on a range of a file.

relocation
	The process of moving block groups within the filesystem while
	maintaining full filesystem integrity and consistency. This
	functionality is underlying *balance* and *device* removing features.

:doc:`scrub<Scrub>`
	An *online* filesystem checking tool. Reads all the data and metadata
        on the filesystem, verifies *checksums* and eventually uses redundant
        copies from *RAID* or *DUP* repair any corrupt data/metadata.

:doc:`seed device<Seeding-device>`
	A readonly device can be used as a filesystem seed or template (e.g. a
	CD-ROM containing an OS image). Read/write devices can be added to
	store modifications (using *copy on write*), changes to the writable
	devices are persistent across reboots. The original device remains
	unchanged and can be removed at any time (after Btrfs has been
	instructed to copy over all missing blocks). Multiple read/write file
	systems can be built from the same seed.

single
	A block group profile storing a single copy of each piece of data.

:doc:`snapshot<Subvolumes>`
	A *subvolume* which is a *copy on write* copy of another subvolume. The
	two subvolumes share all of their common (unmodified) data, which means
	that snapshots can be used to keep the historical state of a filesystem
	very cheaply. After the snapshot is made, the original subvolume and
	the snapshot are of equal status: the original does not "own" the
	snapshot, and either one can be deleted without affecting the other
	one.

:doc:`subvolume<Subvolumes>`
	A tree of files and directories inside a btrfs that can be mounted as
	if it were an independent filesystem. A subvolume is created by taking
	a reference on the root of another subvolume. Each btrfs filesystem has
	at least one subvolume, the *top-level subvolume*, which contains
	everything else in the filesystem. Additional subvolumes can be created
        and deleted with the *btrfs<* tool. All subvolumes share the same pool
        of free space in the filesystem. See also *default subvolume*.

super block
        A special metadata block that is a main access point of the filesystem
        structures. It's size is fixed and there are fixed locations on the devices
        used for detecting and opening the filesystem. Updating the superblock
        defines one *transaction*. The super blocks contains filesystem
        identification (UUID), checksum type, block pointers to fundamental
        trees, features and creation parameters.

system array
        A technical term for *super block* metadata describing how to assemble a
	filesystem from multiple device, storing information about chunks and devices that are
        required to be scanned/registered at the time the mount happens.
        Scanning is done by command :command:`btrfs device scan`, alternatively
        all the required devices can be specified by a mount option *device=/path*.

top-level subvolume
	The *subvolume* at the very top of the filesystem. This is the only
	subvolume present in a newly-created btrfs filesystem, and internally has ID 5,
	otherwise could be referenced as 0 (e.g. within the *set-default* subcommand of
	*btrfs*).

transaction
	A consistent set of changes. To avoid generating very large amounts of
	disk activity, btrfs caches changes in RAM for up to 30 seconds
	(sometimes more often if the filesystem is running short on space or
	doing a lot of *fsync*s), and then writes (commits) these changes out
	to disk in one go (using *copy on write* behaviour). This period of
	caching is called a transaction. Only one transaction is active on the
	filesystem at any one time.

transid
	An alternative term for *generation*.

writeback
	*Writeback* in the context of the Linux kernel can be defined as the
	process of writing "dirty" memory from the page cache to the disk,
	when certain conditions are met (timeout, number of dirty pages over a
	ratio).

..
        TODO (hidden)
        delayed allocation
