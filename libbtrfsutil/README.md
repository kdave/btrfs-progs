libbtrfsutil
============

libbtrfsutil is a library for managing Btrfs filesystems. It is licensed under
the LGPL. libbtrfsutil provides interfaces for a subset of the operations
offered by the `btrfs` command line utility. It also includes official Python
bindings (Python 3 only).

API Overview
------------

This section provides an overview of the interfaces available in libbtrfsutil
as well as example usages. Detailed documentation for the C API can be found in
[`btrfsutil.h`](btrfsutil.h). Detailed documentation for the Python bindings is
available with `pydoc3 btrfsutil` or in the interpreter:

```
>>> import btrfsutil
>>> help(btrfsutil)
```

Many functions in the C API have a variant taking a path and a variant taking a
file descriptor. The latter has the same name as the former with an `_fd`
suffix. The Python bindings for these functions can take a path, a file object,
or a file descriptor.

Error handling is omitted from most of these examples for brevity. Please
handle errors in production code.

### Error Handling

In the C API, all functions that can return an error return an `enum
btrfs_util_error` and set `errno`. `BTRFS_UTIL_OK` (zero) is returned on
success. `btrfs_util_strerror()` converts an error code to a string
description suitable for human-friendly error reporting.

```c
enum btrfs_util_err err;

err = btrfs_util_sync("/");
if (err)
	fprintf(stderr, "%s: %m\n", btrfs_util_strerror(err));
```

In the Python bindings, functions may raise a `BtrfsUtilError`, which is a
subclass of `OSError` with an added `btrfsutilerror` error code member. Error
codes are available as `ERROR_*` constants.

```python
try:
    btrfsutil.sync('/')
except btrfsutil.BtrfsUtilError as e:
    print(e, file=sys.stderr)
```

### Filesystem Operations

There are several operations which act on the entire filesystem.

#### Sync

Btrfs can commit all caches for a specific filesystem to disk.

`btrfs_util_sync()` forces a sync on the filesystem containing the given file
and waits for it to complete.

`btrfs_wait_sync()` waits for a previously started transaction to complete. The
transaction is specified by ID, which may be zero to indicate the current
transaction.

`btrfs_start_sync()` asynchronously starts a sync and returns a transaction ID
which can then be passed to `btrfs_wait_sync()`.

```c
uint64_t transid;
btrfs_util_sync("/");
btrfs_util_start_sync("/", &transid);
btrfs_util_wait_sync("/", &transid);
btrfs_util_wait_sync("/", 0);
```

```python
btrfsutil.sync('/')
transid = btrfsutil.start_sync('/')
btrfsutil.wait_sync('/', transid)
btrfsutil.wait_sync('/')  # equivalent to wait_sync('/', 0)
```

All of these functions have `_fd` variants.

The equivalent `btrfs-progs` command is `btrfs filesystem sync`.

### Subvolume Operations

Functions which take a file and a subvolume ID can be used in two ways. If zero
is given as the subvolume ID, then the given file is used as the subvolume.
Otherwise, the given file can be any file in the filesystem, and the subvolume
with the given ID is used.

#### Subvolume Information

`btrfs_util_is_subvolume()` returns whether a given file is a subvolume.

`btrfs_util_subvolume_id()` returns the ID of the subvolume containing the
given file.

```c
enum btrfs_util_error err;
err = btrfs_util_is_subvolume("/subvol");
if (!err)
	printf("Subvolume\n");
else if (err == BTRFS_UTIL_ERROR_NOT_BTRFS || err == BTRFS_UTIL_ERROR_NOT_SUBVOLUME)
	printf("Not subvolume\n");
uint64_t id;
btrfs_util_subvolume_id("/subvol", &id);
```

```python
if btrfsutil.is_subvolume('/subvol'):
    print('Subvolume')
else:
    print('Not subvolume')
id_ = btrfsutil.subvolume_id('/subvol')
```

`btrfs_util_subvolume_path()` returns the path of the subvolume with the given
ID relative to the filesystem root. This requires `CAP_SYS_ADMIN`. The path
must be freed with `free()`.

```c
char *path;
btrfs_util_subvolume_path("/", 256, &path);
free(path);
btrfs_util_subvolume_path("/subvol", 0, &path);
free(path);
```

```python
path = btrfsutil.subvolume_path('/', 256)
path = btrfsutil.subvolume_path('/subvol')  # equivalent to subvolume_path('/subvol', 0)
```

`btrfs_util_subvolume_info()` returns information (including ID, parent ID,
UUID) about a subvolume. In the C API, this is returned as a `struct
btrfs_util_subvolume_info`. The Python bindings use a `SubvolumeInfo` object.

This requires `CAP_SYS_ADMIN` unless the given subvolume ID is zero and the
kernel supports the `BTRFS_IOC_GET_SUBVOL_INFO` ioctl (added in 4.18).

The equivalent `btrfs-progs` command is `btrfs subvolume show`.

```c
struct btrfs_util_subvolume_info info;
btrfs_util_subvolume_info("/", 256, &info);
btrfs_util_subvolume_info("/subvol", 0, &info);
```

```python
info = btrfsutil.subvolume_info('/', 256)
info = btrfsutil.subvolume_info('/subvol')  # equivalent to subvolume_info('/subvol', 0)
```

All of these functions have `_fd` variants.

#### Enumeration

An iterator interface is provided for enumerating subvolumes on a filesystem.
In the C API, a `struct btrfs_util_subvolume_iterator` is initialized by
`btrfs_util_create_subvolume_iterator()`, which takes a top subvolume to
enumerate under and flags. Currently, the only flag is to specify post-order
traversal instead of the default pre-order. This function has an `_fd` variant.

`btrfs_util_destroy_subvolume_iterator()` must be called to free a previously
created `struct btrfs_util_subvolume_iterator`.

`btrfs_util_subvolume_iterator_fd()` returns the file descriptor opened by
`btrfs_util_create_subvolume_iterator()` which can be used for other functions.

`btrfs_util_subvolume_iterator_next()` returns the path (relative to the top
subvolume that the iterator was created with) and ID of the next subvolume.
`btrfs_util_subvolume_iterator_next_info()` returns a `struct
btrfs_subvolume_info` instead of the ID. It is slightly more efficient than
doing separate `btrfs_util_subvolume_iterator_next()` and
`btrfs_util_subvolume_info()` calls if the subvolume information is needed. The
path returned by these functions must be freed with `free()`. When there are no
more subvolumes, they return `BTRFS_UTIL_ERROR_STOP_ITERATION`.

```c
struct btrfs_util_subvolume_iterator *iter;
enum btrfs_util_error err;
char *path;
uint64_t id;
struct btrfs_util_subvolume_info info;

btrfs_util_create_subvolume_iterator("/", 256, 0, &iter);
/*
 * This is just an example use-case for btrfs_util_subvolume_iterator_fd(). It
 * is not necessary.
 */
btrfs_util_sync_fd(btrfs_util_subvolume_iterator_fd(iter));
while (!(err = btrfs_util_subvolume_iterator_next(iter, &path, &id))) {
	printf("%" PRIu64 " %s\n", id, path);
	free(path);
}
btrfs_util_destroy_subvolume_iterator(iter);

btrfs_util_create_subvolume_iterator("/subvol", 0,
				     BTRFS_UTIL_SUBVOLUME_ITERATOR_POST_ORDER,
				     &iter);
while (!(err = btrfs_util_subvolume_iterator_next_info(iter, &path, &info))) {
	printf("%" PRIu64 " %" PRIu64 " %s\n", info.id, info.parent_id, path);
	free(path);
}
btrfs_util_destroy_subvolume_iterator(iter);
```

The Python bindings provide this interface as an iterable `SubvolumeIterator`
class. It should be used as a context manager to ensure that the underlying
file descriptor is closed. Alternatively, it has a `close()` method for closing
explicitly. It also has a `fileno()` method to get the underlying file
descriptor.

```python
with btrfsutil.SubvolumeIterator('/', 256) as it:
    # This is just an example use-case for fileno(). It is not necessary.
    btrfsutil.sync(it.fileno())
    for path, id_ in it:
        print(id_, path)

it = btrfsutil.SubvolumeIterator('/subvol', info=True, post_order=True)
try:
    for path, info in it:
        print(info.id, info.parent_id, path)
finally:
    it.close()
```

This interface requires `CAP_SYS_ADMIN` unless the given top subvolume ID is
zero and the kernel supports the `BTRFS_IOC_GET_SUBVOL_ROOTREF` and
`BTRFS_IOC_INO_LOOKUP_USER` ioctls (added in 4.18). In the unprivileged case,
subvolumes which cannot be accessed are skipped.

The equivalent `btrfs-progs` command is `btrfs subvolume list`.

#### Creation

`btrfs_util_create_subvolume()` creates a new subvolume at the given path. The
subvolume can inherit from quota groups (qgroups).

Qgroups to inherit are specified with a `struct btrfs_util_qgroup_inherit`,
which is created by `btrfs_util_create_qgroup_inherit()` and freed by
`btrfs_util_destroy_qgroup_inherit()`. Qgroups are added with
`btrfs_util_qgroup_inherit_add_group()`. The list of added groups can be
retrieved with `btrfs_util_qgroup_inherit_get_groups()`; note that the returned
array does not need to be freed and is no longer valid when the `struct
btrfs_util_qgroup_inherit` is freed.

The Python bindings provide a `QgroupInherit` class. It has an `add_group()`
method and a `groups` member, which is a list of ints.

```c
btrfs_util_create_subvolume("/subvol2", 0, NULL, NULL);

struct btrfs_util_qgroup_inherit *qgroups;
btrfs_util_create_qgroup_inherit(0, &qgroups);
btrfs_util_qgroup_inherit_add_group(&qgroups, 256);
btrfs_util_create_subvolume("/subvol2", 0, NULL, qgroups);
btrfs_util_destroy_qgroup_inherit(qgroups);
```

```python
btrfsutil.create_subvolume('/subvol2')

qgroups = btrfsutil.QgroupInherit()
qgroups.add_group(256)
btrfsutil.create_subvolume('/subvol2', qgroup_inherit=qgroups)
```

The C API has an `_fd` variant which takes a name and a file descriptor
referring to the parent directory.

The equivalent `btrfs-progs` command is `btrfs subvolume create`.

#### Snapshotting

Snapshots are created with `btrfs_util_create_snapshot()`, which takes a source
path, a destination path, and flags. It can also inherit from quota groups;
see [subvolume creation](#Creation).

Snapshot creation can be recursive, in which case subvolumes underneath the
subvolume being snapshotted will also be snapshotted onto the same location in
the new snapshot (note that this is implemented in userspace non-atomically and
has the same capability requirements as a [subvolume iterator](#Enumeration)).
The newly created snapshot can also be read-only, but not if doing a recursive
snapshot.

```c
btrfs_util_create_snapshot("/subvol", "/snapshot", 0, NULL, NULL);
btrfs_util_create_snapshot("/nested_subvol", "/nested_snapshot",
			   BTRFS_UTIL_CREATE_SNAPSHOT_RECURSIVE, NULL, NULL);
btrfs_util_create_snapshot("/subvol", "/rosnapshot",
			   BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY, NULL, NULL);
```

```python
btrfsutil.create_snapshot('/subvol', '/snapshot')
btrfsutil.create_snapshot('/nested_subvol', '/nested_snapshot', recursive=True)
btrfsutil.create_snapshot('/subvol', '/rosnapshot', read_only=True)
```

The C API has two `_fd` variants. `btrfs_util_create_snapshot_fd()` takes the
source subvolume as a file descriptor. `btrfs_util_create_snapshot_fd2()` takes
the source subvolume as a file descriptor and the destination as a name and
parent file descriptor.

The equivalent `btrfs-progs` command is `btrfs subvolume snapshot`.

#### Deletion

`btrfs_util_delete_subvolume()` takes a subvolume to delete and flags. This
requires `CAP_SYS_ADMIN` if the filesystem was not mounted with
`user_subvol_rm_allowed`. Deletion may be recursive, in which case all
subvolumes beneath the given subvolume are deleted before the given subvolume
is deleted. This is implemented in user-space non-atomically and has the same
capability requirements as a [subvolume iterator](#Enumeration).

```c
btrfs_util_delete_subvolume("/subvol", 0);
btrfs_util_delete_subvolume("/nested_subvol",
			    BTRFS_UTIL_DELETE_SUBVOLUME_RECURSIVE);
```

```python
btrfsutil.delete_subvolume('/subvol')
btrfsutil.delete_subvolume('/nested_subvol', recursive=True)
```

The C API has an `_fd` variant which takes a name and a file descriptor
referring to the parent directory.

The equivalent `btrfs-progs` command is `btrfs subvolume delete`.

#### Deleted Subvolumes

Btrfs lazily cleans up deleted subvolumes. `btrfs_util_deleted_subvolumes()`
returns an array of subvolume IDs which have been deleted but not yet cleaned
up. The returned array should be freed with `free()`.
```c
uint64_t *ids;
size_t n; /* Number of returned IDs. */
btrfs_util_deleted_subvolumes("/", &ids, &n);
free(ids);
```

The Python binding returns a list of ints.

```python
ids = btrfsutil.deleted_subvolumes('/')
```

This function also has an `_fd` variant. It requires `CAP_SYS_ADMIN`.

The closest `btrfs-progs` command is `btrfs subvolume sync`, which waits for
deleted subvolumes to be cleaned up.

#### Read-Only Flag

Subvolumes can be set to read-only. `btrfs_util_get_subvolume_read_only()`
returns whether a subvolume is read-only.
`btrfs_util_set_subvolume_read_only()` sets the read-only flag to the desired
value.

```c
bool read_only;
btrfs_util_get_subvolume_read_only("/subvol", &read_only);
btrfs_util_set_subvolume_read_only("/subvol", true);
btrfs_util_set_subvolume_read_only("/subvol", false);
```

```python
read_only = btrfsutil.get_subvolume_read_only('/subvol')
btrfsutil.set_subvolume_read_only('/subvol', True)
btrfsutil.set_subvolume_read_only('/subvol', False)
```

Both of these functions have `_fd` variants.

The equivalent `btrfs-progs` commands are `btrfs property get` and `btrfs
property set` with the `ro` property.

#### Default Subvolume

The default subvolume of a filesystem is the subvolume which is mounted when no
`subvol` or `subvolid` mount option is passed.

`btrfs_util_get_default_subvolume()` gets the ID of the default subvolume for
the filesystem containing the given file.

`btrfs_util_set_default_subvolume()` sets the default subvolume.

```c
uint64_t id;
btrfs_util_get_default_subvolume("/", &id);
btrfs_util_set_default_subvolume("/", 256);
btrfs_util_set_default_subvolume("/subvol", 0);
```

```python
id = btrfsutil.get_default_subvolume('/')
btrfsutil.set_default_subvolume('/', 256)
btrfsutil.set_default_subvolume('/subvol')  # equivalent to set_default_subvolume('/subvol', 0)
```

Both of these functions have an `_fd` variant. They both require
`CAP_SYS_ADMIN`.

The equivalent `btrfs-progs` commands are `btrfs subvolume get-default` and
`btrfs subvolume set-default`.

Development
-----------

The [development process for btrfs-progs](../README.md#development) applies.

libbtrfsutil only includes operations that are done through the filesystem and
ioctl interface, not operations that modify the filesystem directly (e.g., mkfs
or fsck). This is by design but also a legal necessity, as the filesystem
implementation is GPL but libbtrfsutil is LGPL. That is also why the
libbtrfsutil code is a reimplementation of the btrfs-progs code rather than a
refactoring. Be wary of this when porting functionality.

libbtrfsutil is semantically versioned separately from btrfs-progs. It is the
maintainers' responsibility to bump the version as needed (at most once per
release of btrfs-progs).

A few guidelines:

* All interfaces must be documented in this README and in `btrfsutil.h` using
  the kernel-doc style
* Error codes should be specific about what _exactly_ failed
* Functions should have a path and an fd variant whenever possible
* Spell out terms in function names, etc. rather than abbreviating whenever
  possible
* Don't require the Btrfs UAPI headers for any interfaces (e.g., instead of
  directly exposing a type from `linux/btrfs_tree.h`, abstract it away in a
  type specific to `libbtrfsutil`)
* Preserve API and ABI compatibility at all times (i.e., we don't want to bump
  the library major version if we don't have to)
* Include Python bindings for all interfaces
* Write tests for all interfaces

### Extending API

Adding a new function to the API requires updating several locations scattered
everywhere. The following checklist should help to make sure nothing is missing:

* `libbtrfsutil/btrfsutil.h` add exported functions, with proper documentation
  following examples of the others (documented parameters, behaviour, return
  values, other relevant information/quirks)
* `libbtrfsutil/btrfsutil.h` add any new error 'btrfs\_util\_error' enums
  specific to the added functions and in `libbtrfsutil/errors.c` write text
  descriptions
* `libbtrfsutil/btrfsutil.h` add new constants if necessary, new values must be
  defined even if there's already an existing one in another 'btrfs-progs' header,
  prefix them with 'BTRFS\_UTIL\_'
* implementation goes to `*.c`, existing one if the class of the API already
  exists or create a new one, in that case update `Makefile` and variable
  'libbtrfsutil\_objects'
* `libbtrfsutil.sym` add new exported symbols, add a new versioned section if
  necessary, bump minor version
* `python/btrfsutilpy.h` declare C functions implementing the binding
* `python/*.c` add the implementation, filenames follow the library '\*.c',
  follow examples of other functions how the bindings are done, this can be the
  hard part in case there are non-trivial return values
* `python/module.c` add binding description entry for the new functions
* `python/tests/test_*.py` write test for the new functionality
* `README.md` add documentation for the new functions

### API summary

* filesystem
  * sync
  * wait for sync
* subvolume
  * create
  * delete
  * is subvolume
  * get containing subvolume id
  * get path of id
  * get info
  * set/get default
  * set/get read-only flag
  * list (live and deleted)
* qgroups
  * create
  * inherit
  * add relation
  * destroy
