Glossary
========

Terms in *italics* also appear in this glossary.

allocator
	Usually *allocator* means the *block* allocator, ie. the logic
	inside filesystem which decides where to place newly allocated blocks
	in order to maintain several constraints (like data locality, low
	fragmentation).

	In btrfs, allocator may also refer to *chunk* allocator, ie. the
	logic behind placing chunks on devices.

balance
	An operation that can be done to a btrfs filesystem, for example
	through <code>btrfs fi balance /path</code> (see *btrfs-progs*). A
	balance passes all data in the filesystem through the *allocator*
	again. It is primarily intended to rebalance the data in the filesystem
	across the *devices* when a device is added or removed. A balance
	will regenerate missing copies for the redundant *RAID* levels, if a
	device has failed. As of linux kernel 3.3, a balance operation can be
	made selective about which parts of the filesystem are rewritten.

barrier
	An instruction to the disk hardware to ensure that everything before
	the barrier is physically written to permanent storage before anything
	after it. Used in btrfs's *copy on write* approach to ensure
	filesystem consistency.

block
	A single physically and logically contiguous piece of storage on a
	device, of size eg. 4K.

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
	categories of B-tree. For reference, see [[Btrees]]. The name "btrfs"
	refers to its use of B-trees.

btrfsck
	Tool in *btrfs-progs* that checks a filesystem *offline* (ie.
	unmounted), and reports on any errors in the filesystem structures it
	finds. Does not ([[FAQ#When_will_Btrfs_have_a_fsck_like_tool.3F|yet]])
	fix errors by default. Recently it got support to fix certain types of
	corruption. See also *scrub*.

btrfs-progs
	User mode tools to manage btrfs-specific features. Maintained at
	[http://git.kernel.org/?p=linux/kernel/git/mason/btrfs-progs.git;a=summary|btrfs-progs
	gitweb]. The main frontend to btrfs features is the
	<code>[[Manpage/btrfs|btrfs]]</code> program, although other tools such
	as *mkfs.btrfs* and *btrfsck* are also part of btrfs-progs.

chunk
	A part of a *block group*. Chunks are either 1 GiB in size (for data)
	or 256 MiB (for *metadata*).

chunk tree
	A layer that keeps information about mapping between physical and
	logical block addresses. It's stored within the *System* group.

cleaner
	Usually referred to in context of deleted subvolumes. It's a background
	process that removes the actual data once a subvolume has been deleted.
	Cleaning can involve lots of IO and CPU activity depending on the
	fragmentation and amount of shared data with other subvolumes.

copy-on-write
	Also known as *COW*. The method that btrfs uses for modifying data.
	Instead of directly overwriting data in place, btrfs takes a copy of
	the data, alters it, and then writes the modified data back to a
	different (free) location on the disk. It then updates the *metadata*
	to reflect the new location of the data. In order to update the
	metadata, the affected metadata blocks are also treated in the same
	way. In COW filesystems, files tend to fragment as they are modified.
	Copy-on-write is also used in the implementation of *snapshots* and
	*reflink copies*. A copy-on-write filesystem is, in theory,
	*'always*' consistent, provided the underlying hardware supports
	*barriers*.

COW
	See *copy-on-write*.

default subvolume
	The *subvolume* in a btrfs filesystem which is mounted when mounting
	the filesystem without using the <code>subvol=</code> [[Mount
	options|mount option]].

device
	A Linux block device, e.g. a whole disk, partition, LVM logical volume,
	loopback device, or network block device. A btrfs filesystem can reside
	on one or more devices.

df
	A standard Unix tool for reporting the amount of space used and free in
	a filesystem. The standard tool does not give accurate results, but the
	<code>[[Manpage/btrfs|btrfs]]</code> command from *btrfs-progs* has
	an implementation of df which shows space available in more detail. See
	the
	[[FAQ#Why_does_df_show_incorrect_free_space_for_my_RAID_volume.3F|FAQ]]
	for a more detailed explanation of btrfs free space accounting.

DUP
	A form of "*RAID*" which stores two copies of each piece of data on
	the same *device*. This is similar to *RAID-1*, and protects
	against *block*-level errors on the device, but does not provide any
	guarantees if the entire device fails. By default, btrfs uses *'DUP*'
	profile for metadata on filesystems with one rotational device,
	*'single*' profile on filesystems with one non-rotational device, and
	*'RAID1*' profile on filesystems with more than one device.

ENOSPC
	Error code returned by the OS to a user program when the filesystem
	cannot allocate enough data to fulfill the user requested. In most
	filesystems, it indicates there is no free space available in the
	filesystem. Due to the additional space requirements from btrfs's
	*COW* behaviour, btrfs can sometimes return ENOSPC when there is
	apparently (in terms of *df*) a large amount of space free. This is
	effectively a bug in btrfs, and (if it is repeatable), using the mount
	option <code>[[Mount options|enospc_debug]]</code> may give a report
	that will help the btrfs developers. See the
	[[FAQ#if_your_device_is_large_.28.3E16GiB.29|FAQ entry]] on free space.

extent
	Contiguous sequence of bytes on disk that holds file data.

	A file stored on disk with 3 extents means that it consists of three
	fragments of contiguous bytes. See *filefrag*. A file in one extent
	would mean it is not fragmented.

Extent buffer
	An abstraction to allow access to *B-tree* blocks larger than a page size.

fallocate
	Command line tool in util-linux, and a syscall, that reserves space in
	the filesystem for a file, without actually writing any file data to
	the filesystem. First data write will turn the preallocated extents
	into regular ones. See <code>man 1 fallocate</code> and <code>man 2
	fallocate</code> for more details.

filefrag
	A tool to show the number of extents in a file, and hence the amount of
	fragmentation in the file. It is usually part of the e2fsprogs package
	on most Linux distributions. While initially developed for the ext2
	filesystem, it works on Btrfs as well (but
	[http://thread.gmane.org/gmane.comp.file-systems.ocfs2.devel/8894/focus=8902
	not really with compressed files]). It uses the *FIEMAP* ioctl.

free space cache
	Btrfs doesn't track free space, it only tracks allocated space. Free
	space is by definition any holes in the allocated space, but finding
	these holes is actually fairly I/O intensive. The free space cache
	stores a compressed representation of what is free. It is updated on
	every *transaction* commit.

fsync
	On Unix and Unix-like operating systems (of which Linux is the latter),
	the <code>fsync()</code> system call causes all buffered file
	descriptor related data changes to be flushed to the underlying block
	device. When a file is modified on a modern operating system the
	changes are generally not written to the disk immediately but rather
	those changes are buffered in memory for reasons of performance,
	calling <code>fsync()</code> causes any in-memory changes to be written
	to disk.

generation
	An internal counter which updates for each *transaction*. When a
	*metadata* block is written (using *copy on write*), current
	generation is stored in the block, so that blocks which are too new
	(and hence possibly inconsistent) can be identified.

genid
	See *generation*.

Key
	A fixed sized tuple used to identify and sort items in a *B-tree*.
	The key is broken up into 3 parts: *'objectid*', *'type*', and
	*'offset*'. The *'type*' field indicates how each of the other two
	fields should be used, and what to expect to find in the item. For
	reference, see [[Btree Keys]].

Item
	A variable sized structure stored in B-tree leaves. Items hold
	different types of data depending on key type. For reference, see
	[[Btree Items]].

log tree


metadata
	Data about data. In btrfs, this includes all of the internal data
	structures of the filesystem, including directory structures,
	filenames, file permissions, checksums, and the location of each file's
	*extents*. All btrfs metadata is stored in *B-trees*.

mkfs.btrfs
	The tool (from *btrfs-progs*) to create a btrfs filesystem, see
	[[mkfs.btrfs]].

offline
	A filesystem which is not mounted is offline. Some tools (e.g.
	*btrfsck*) will only work on offline filesystems. Compare *online*.

online
	A filesystem which is mounted is online. Most btrfs tools will only
	work on online filesystems. Compare *offline*.

orphan
	(file)

RAID
	A class of different methods for writing some additional redundant data
	across multiple *devices* so that if one device fails, the missing
	data can be reconstructed from the remaining ones. See *RAID-0*,
	*RAID-1*, *RAID-5*, *RAID-6*, *RAID-10*, *DUP* and
	*single*. Traditional RAID methods operate across multiple devices of
	equal size, whereas btrfs's RAID implementation works inside *block
	groups*. See the [[SysadminGuide#Data_usage_and_allocation|Sysadmin's
	Guide]] for the details.

RAID-0
	A form of *RAID* which provides no form of error recovery, but
	stripes a single copy of data across multiple devices for performance
	purposes. The stripe size is fixed to 64KB for now.

RAID-1
	A form of *RAID* which stores two complete copies of each piece of
	data. Each copy is stored on a different *device*. btrfs requires a
	minimum of two devices to use RAID-1. This is the default for btrfs's
	*metadata* on more than one device.

RAID-5
	A form of *RAID* which stripes a single copy of data across multiple
	*devices*, including one device's worth of additional parity data.
	Can be used to recover from a single device failure. Not yet
	implemented in btrfs.

RAID-6
	A form of *RAID* which stripes a single copy of data across multiple
	*devices*, including two device's worth of additional parity data. Can
	be used to recover from the failure of two devices. Not yet implemented
	in btrfs.

RAID-10
	A form of *RAID* which stores two complete copies of each piece of
	data, and also stripes each copy across multiple devices for
	performance.

reflink
	Parameter to <code>cp</code>, allowing it to take advantage of the
	capabilities of *COW*-capable filesystems. Allows for files to be
	copied and modified, with only the modifications taking up additional
	storage space. May be considered as *snapshots* on a single file rather
	than a *subvolume*. Example: <code>cp --reflink file1 file2</code>

relocation
	The process of moving block groups within the filesystem while
	maintaining full filesystem integrity and consistency. This
	functionality is underlying *balance* and *device* removing features.

restriper
	A development name for the rewritten *balance* code implemented in the
	v3.3 kernel. Allows to change RAID profiles of the filesystem,
	*online*.

scrub
	An *online* filesystem checking tool. Reads all the data and metadata
	on the filesystem, and uses *checksums* and the duplicate copies from
	*RAID* storage to identify and repair any corrupt data.

seed device
	A readonly device can be used as a filesystem seed or template (e.g. a
	CD-ROM containing an OS image). Read/write devices can be added to
	store modifications (using *copy on write*), changes to the writable
	devices are persistent across reboots. The original device remains
	unchanged and can be removed at any time (after Btrfs has been
	instructed to copy over all missing blocks). Multiple read/write file
	systems can be built from the same seed. See [[Seed-device]] for an
	example.

single
	A "*RAID*" level in btrfs, storing a single copy of each piece of data.
	The default for data (as opposed to *metadata*) in btrfs. Single is
	also default metadata profile for non-rotational (SSD, flash) devices.

snapshot
	A *subvolume* which is a *copy on write* copy of another subvolume. The
	two subvolumes share all of their common (unmodified) data, which means
	that snapshots can be used to keep the historical state of a filesystem
	very cheaply. After the snapshot is made, the original subvolume and
	the snapshot are of equal status: the original does not "own" the
	snapshot, and either one can be deleted without affecting the other
	one.

subvolume
	A tree of files and directories inside a btrfs that can be mounted as
	if it were an independent filesystem. A subvolume is created by taking
	a reference on the root of another subvolume. Each btrfs filesystem has
	at least one subvolume, the *top-level subvolume*, which contains
	everything else in the filesystem. Additional subvolumes can be created
	and deleted with the *<code>btrfs</code>* tool. All subvolumes share
	the same pool of free space in the filesystem. See also *default
	subvolume*.

superblock
	The *block* on the disk, at a fixed known location and of fixed size,
	which contains pointers to the disk blocks containing all the other
	filesystem *metadata* structures. btrfs stores multiple copies of the
	superblock on each *device* in the filesystem at offsets 64 KiB, 64
	MiB, 256 GiB, 1 TiB and PiB.

system array
	Cryptic name of *superblock* metadata describing how to assemble a
	filesystem from multiple device. Prior to mount, the command *btrfs dev
	scan* has to be called, or all the devices have to be specified via
	mount option *device=/dev/ice*.

top-level subvolume
	The *subvolume* at the very top of the filesystem. This is the only
	subvolume present in a newly-created btrfs filesystem, and internally has ID 5,
	otherwise could be referenced as 0 (eg. within the *set-default* subcommand of
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
	An alternative term for *genid*. See *generation*.

writeback
	*Writeback* in the context of the Linux kernel can be defined as the
	process of writing "dirty" memory from the page cache to the disk,
	when certain conditions are met (timeout, number of dirty pages over a
	ratio).
