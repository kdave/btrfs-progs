/*
 * Copyright (C) 2018 Facebook
 *
 * This file is part of libbtrfsutil.
 *
 * libbtrfsutil is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * libbtrfsutil is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libbtrfsutil.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef BTRFS_UTIL_H
#define BTRFS_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

#define BTRFS_UTIL_VERSION_MAJOR 1
#define BTRFS_UTIL_VERSION_MINOR 2
#define BTRFS_UTIL_VERSION_PATCH 0

#ifdef __cplusplus
extern "C" {
#endif

/**
 * enum btrfs_util_error - libbtrfsutil error codes.
 *
 * All functions in libbtrfsutil that can return an error return this type and
 * set errno.
 */
enum btrfs_util_error {
	BTRFS_UTIL_OK,
	BTRFS_UTIL_ERROR_STOP_ITERATION,
	BTRFS_UTIL_ERROR_NO_MEMORY,
	BTRFS_UTIL_ERROR_INVALID_ARGUMENT,
	BTRFS_UTIL_ERROR_NOT_BTRFS,
	BTRFS_UTIL_ERROR_NOT_SUBVOLUME,
	BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND,
	BTRFS_UTIL_ERROR_OPEN_FAILED,
	BTRFS_UTIL_ERROR_RMDIR_FAILED,
	BTRFS_UTIL_ERROR_UNLINK_FAILED,
	BTRFS_UTIL_ERROR_STAT_FAILED,
	BTRFS_UTIL_ERROR_STATFS_FAILED,
	BTRFS_UTIL_ERROR_SEARCH_FAILED,
	BTRFS_UTIL_ERROR_INO_LOOKUP_FAILED,
	BTRFS_UTIL_ERROR_SUBVOL_GETFLAGS_FAILED,
	BTRFS_UTIL_ERROR_SUBVOL_SETFLAGS_FAILED,
	BTRFS_UTIL_ERROR_SUBVOL_CREATE_FAILED,
	BTRFS_UTIL_ERROR_SNAP_CREATE_FAILED,
	BTRFS_UTIL_ERROR_SNAP_DESTROY_FAILED,
	BTRFS_UTIL_ERROR_DEFAULT_SUBVOL_FAILED,
	BTRFS_UTIL_ERROR_SYNC_FAILED,
	BTRFS_UTIL_ERROR_START_SYNC_FAILED,
	BTRFS_UTIL_ERROR_WAIT_SYNC_FAILED,
	BTRFS_UTIL_ERROR_GET_SUBVOL_INFO_FAILED,
	BTRFS_UTIL_ERROR_GET_SUBVOL_ROOTREF_FAILED,
	BTRFS_UTIL_ERROR_INO_LOOKUP_USER_FAILED,
	BTRFS_UTIL_ERROR_FS_INFO_FAILED,
};

/**
 * btrfs_util_strerror() - Convert a libtrfsutil error code to a string
 * description.
 * @err: The error to convert.
 *
 * Return: Error description.
 */
const char *btrfs_util_strerror(enum btrfs_util_error err);

/**
 * btrfs_util_sync() - Force a sync on a specific Btrfs filesystem.
 * @path: Path on a Btrfs filesystem.
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_sync(const char *path);

/**
 * btrfs_util_sync_fd() - See btrfs_util_sync().
 */
enum btrfs_util_error btrfs_util_sync_fd(int fd);

/**
 * btrfs_util_start_sync() - Start a sync on a specific Btrfs filesystem but
 * don't wait for it.
 * @path: Path on a Btrfs filesystem.
 * @transid: Returned transaction ID which can be waited on with
 * btrfs_util_wait_sync(). This can be %NULL.
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_start_sync(const char *path,
					    uint64_t *transid);

/**
 * btrfs_util_start_sync_fd() - See btrfs_util_start_sync().
 */
enum btrfs_util_error btrfs_util_start_sync_fd(int fd, uint64_t *transid);

/**
 * btrfs_util_wait_sync() - Wait for a transaction with a given ID to sync.
 * @path: Path on a Btrfs filesystem.
 * @transid: Transaction ID to wait for, or zero for the current transaction.
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_wait_sync(const char *path, uint64_t transid);

/**
 * btrfs_util_wait_sync_fd() - See btrfs_util_wait_sync().
 */
enum btrfs_util_error btrfs_util_wait_sync_fd(int fd, uint64_t transid);

/**
 * btrfs_util_is_subvolume() - Return whether a given path is a Btrfs subvolume.
 * @path: Path to check.
 *
 * Return: %BTRFS_UTIL_OK if @path is a Btrfs subvolume,
 * %BTRFS_UTIL_ERROR_NOT_BTRFS if @path is not on a Btrfs filesystem,
 * %BTRFS_UTIL_ERROR_NOT_SUBVOLUME if @path is not a subvolume, non-zero error
 * code on any other failure.
 */
enum btrfs_util_error btrfs_util_is_subvolume(const char *path);

/**
 * btrfs_util_is_subvolume_fd() - See btrfs_util_is_subvolume().
 */
enum btrfs_util_error btrfs_util_is_subvolume_fd(int fd);

/**
 * btrfs_util_subvolume_id() - Get the ID of the subvolume containing a path.
 * @path: Path on a Btrfs filesystem.
 * @id_ret: Returned subvolume ID.
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_subvolume_id(const char *path,
					      uint64_t *id_ret);

/**
 * btrfs_util_subvolume_id_fd() - See btrfs_util_subvolume_id().
 */
enum btrfs_util_error btrfs_util_subvolume_id_fd(int fd, uint64_t *id_ret);

/**
 * btrfs_util_subvolume_path() - Get the path of the subvolume with a given ID
 * relative to the filesystem root.
 * @path: Path on a Btrfs filesystem.
 * @id: ID of subvolume to set as the default. If zero is given, the subvolume
 * ID of @path is used.
 * @path_ret: Returned path.
 *
 * This requires appropriate privilege (CAP_SYS_ADMIN).
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_subvolume_path(const char *path, uint64_t id,
						char **path_ret);

/**
 * btrfs_util_subvolume_path_fd() - See btrfs_util_subvolume_path().
 */
enum btrfs_util_error btrfs_util_subvolume_path_fd(int fd, uint64_t id,
						   char **path_ret);

/**
 * struct btrfs_util_subvolume_info - Information about a Btrfs subvolume.
 */
struct btrfs_util_subvolume_info {
	/** @id: ID of this subvolume, unique across the filesystem. */
	uint64_t id;

	/**
	 * @parent_id: ID of the subvolume which contains this subvolume, or
	 * zero for the root subvolume (BTRFS_FS_TREE_OBJECTID) or orphaned
	 * subvolumes (i.e., subvolumes which have been deleted but not yet
	 * cleaned up).
	 */
	uint64_t parent_id;

	/**
	 * @dir_id: Inode number of the directory containing this subvolume in
	 * the parent subvolume, or zero for the root subvolume
	 * (BTRFS_FS_TREE_OBJECTID) or orphaned subvolumes.
	 */
	uint64_t dir_id;

	/** @flags: On-disk root item flags. */
	uint64_t flags;

	/** @uuid: UUID of this subvolume. */
	uint8_t uuid[16];

	/**
	 * @parent_uuid: UUID of the subvolume this subvolume is a snapshot of,
	 * or all zeroes if this subvolume is not a snapshot.
	 */
	uint8_t parent_uuid[16];

	/**
	 * @received_uuid: UUID of the subvolume this subvolume was received
	 * from, or all zeroes if this subvolume was not received. Note that
	 * this field, @stransid, @rtransid, @stime, and @rtime are set manually
	 * by userspace after a subvolume is received.
	 */
	uint8_t received_uuid[16];

	/** @generation: Transaction ID of the subvolume root. */
	uint64_t generation;

	/**
	 * @ctransid: Transaction ID when an inode in this subvolume was last
	 * changed.
	 */
	uint64_t ctransid;

	/** @otransid: Transaction ID when this subvolume was created. */
	uint64_t otransid;

	/**
	 * @stransid: Transaction ID of the sent subvolume this subvolume was
	 * received from, or zero if this subvolume was not received. See the
	 * note on @received_uuid.
	 */
	uint64_t stransid;

	/**
	 * @rtransid: Transaction ID when this subvolume was received, or zero
	 * if this subvolume was not received. See the note on @received_uuid.
	 */
	uint64_t rtransid;

	/** @ctime: Time when an inode in this subvolume was last changed. */
	struct timespec ctime;

	/** @otime: Time when this subvolume was created. */
	struct timespec otime;

	/**
	 * @stime: Not well-defined, usually zero unless it was set otherwise.
	 * See the note on @received_uuid.
	 */
	struct timespec stime;

	/**
	 * @rtime: Time when this subvolume was received, or zero if this
	 * subvolume was not received. See the note on @received_uuid.
	 */
	struct timespec rtime;
};

/**
 * btrfs_util_subvolume_info() - Get information about a subvolume.
 * @path: Path in a Btrfs filesystem. This may be any path in the filesystem; it
 * does not have to refer to a subvolume unless @id is zero.
 * @id: ID of subvolume to get information about. If zero is given, the
 * subvolume ID of @path is used.
 * @subvol: Returned subvolume information. This can be %NULL if you just want
 * to check whether the subvolume exists; %BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND
 * will be returned if it does not.
 *
 * This requires appropriate privilege (CAP_SYS_ADMIN) unless @id is zero and
 * the kernel supports BTRFS_IOC_GET_SUBVOL_INFO (kernel >= 4.18).
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_subvolume_info(const char *path, uint64_t id,
						struct btrfs_util_subvolume_info *subvol);

/**
 * btrfs_util_subvolume_info_fd() - See btrfs_util_subvolume_info().
 */
enum btrfs_util_error btrfs_util_subvolume_info_fd(int fd, uint64_t id,
						   struct btrfs_util_subvolume_info *subvol);

/**
 * btrfs_util_get_subvolume_read_only() - Get whether a subvolume is read-only.
 * @path: Subvolume path.
 * @ret: Returned read-only flag.
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_get_subvolume_read_only(const char *path,
							 bool *ret);

/**
 * btrfs_util_get_subvolume_read_only_fd() - See
 * btrfs_util_get_subvolume_read_only().
 */
enum btrfs_util_error btrfs_util_get_subvolume_read_only_fd(int fd, bool *ret);

/**
 * btrfs_util_set_subvolume_read_only() - Set whether a subvolume is read-only.
 * @path: Subvolume path.
 * @read_only: New value of read-only flag.
 *
 * This requires appropriate privilege (CAP_SYS_ADMIN).
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_set_subvolume_read_only(const char *path,
							 bool read_only);

/**
 * btrfs_util_set_subvolume_read_only_fd() - See
 * btrfs_util_set_subvolume_read_only().
 */
enum btrfs_util_error btrfs_util_set_subvolume_read_only_fd(int fd,
							    bool read_only);

/**
 * btrfs_util_get_default_subvolume() - Get the default subvolume for a
 * filesystem.
 * @path: Path on a Btrfs filesystem.
 * @id_ret: Returned subvolume ID.
 *
 * This requires appropriate privilege (CAP_SYS_ADMIN).
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_get_default_subvolume(const char *path,
						       uint64_t *id_ret);

/**
 * btrfs_util_get_default_subvolume_fd() - See
 * btrfs_util_get_default_subvolume().
 */
enum btrfs_util_error btrfs_util_get_default_subvolume_fd(int fd,
							  uint64_t *id_ret);

/**
 * btrfs_util_set_default_subvolume() - Set the default subvolume for a
 * filesystem.
 * @path: Path in a Btrfs filesystem. This may be any path in the filesystem; it
 * does not have to refer to a subvolume unless @id is zero.
 * @id: ID of subvolume to set as the default. If zero is given, the subvolume
 * ID of @path is used.
 *
 * This requires appropriate privilege (CAP_SYS_ADMIN).
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_set_default_subvolume(const char *path,
						       uint64_t id);

/**
 * btrfs_util_set_default_subvolume_fd() - See
 * btrfs_util_set_default_subvolume().
 */
enum btrfs_util_error btrfs_util_set_default_subvolume_fd(int fd, uint64_t id);

struct btrfs_util_qgroup_inherit;

/**
 * btrfs_util_create_subvolume() - Create a new subvolume.
 * @path: Where to create the subvolume.
 * @flags: Must be zero.
 * @async_transid: If not NULL, create the subvolume asynchronously (i.e.,
 * without waiting for it to commit it to disk) and return the transaction ID
 * that it was created in. This transaction ID can be waited on with
 * btrfs_util_wait_sync().
 * @qgroup_inherit: Qgroups to inherit from, or NULL.
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_create_subvolume(const char *path, int flags,
						  uint64_t *async_transid,
						  struct btrfs_util_qgroup_inherit *qgroup_inherit);

/**
 * btrfs_util_create_subvolume_fd() - Create a new subvolume given its parent
 * and name.
 * @parent_fd: File descriptor of the parent directory where the subvolume
 * should be created.
 * @name: Name of the subvolume to create.
 * @flags: See btrfs_util_create_subvolume().
 * @async_transid: See btrfs_util_create_subvolume().
 * @qgroup_inherit: See btrfs_util_create_subvolume().
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_create_subvolume_fd(int parent_fd,
						     const char *name,
						     int flags,
						     uint64_t *async_transid,
						     struct btrfs_util_qgroup_inherit *qgroup_inherit);

/**
 * BTRFS_UTIL_CREATE_SNAPSHOT_RECURSIVE - Also snapshot subvolumes beneath the
 * source subvolume onto the same location on the new snapshot.
 *
 * Note that this is currently implemented in userspace non-atomically. Because
 * it modifies the newly-created snapshot, it cannot be combined with
 * %BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY. It requires appropriate privilege
 * (CAP_SYS_ADMIN).
 */
#define BTRFS_UTIL_CREATE_SNAPSHOT_RECURSIVE (1 << 0)
/**
 * BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY - Create a read-only snapshot.
 */
#define BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY (1 << 1)
#define BTRFS_UTIL_CREATE_SNAPSHOT_MASK ((1 << 2) - 1)

/**
 * btrfs_util_create_snapshot() - Create a new snapshot from a source subvolume
 * path.
 * @source: Path of the existing subvolume to snapshot.
 * @path: Where to create the snapshot.
 * @flags: Bitmask of BTRFS_UTIL_CREATE_SNAPSHOT_* flags.
 * @async_transid: See btrfs_util_create_subvolume(). If
 * %BTRFS_UTIL_CREATE_SNAPSHOT_RECURSIVE was in @flags, then this will contain
 * the largest transaction ID of all created subvolumes.
 * @qgroup_inherit: See btrfs_util_create_subvolume().
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_create_snapshot(const char *source,
						 const char *path, int flags,
						 uint64_t *async_transid,
						 struct btrfs_util_qgroup_inherit *qgroup_inherit);

/**
 * btrfs_util_create_snapshot_fd() - See btrfs_util_create_snapshot().
 */
enum btrfs_util_error btrfs_util_create_snapshot_fd(int fd, const char *path,
						    int flags,
						    uint64_t *async_transid,
						    struct btrfs_util_qgroup_inherit *qgroup_inherit);

/**
 * btrfs_util_create_snapshot_fd2() - Create a new snapshot from a source
 * subvolume file descriptor and a target parent file descriptor and name.
 * @fd: File descriptor of the existing subvolume to snapshot.
 * @parent_fd: File descriptor of the parent directory where the snapshot should
 * be created.
 * @name: Name of the snapshot to create.
 * @flags: See btrfs_util_create_snapshot().
 * @async_transid: See btrfs_util_create_snapshot().
 * @qgroup_inherit: See btrfs_util_create_snapshot().
 */
enum btrfs_util_error btrfs_util_create_snapshot_fd2(int fd, int parent_fd,
						     const char *name,
						     int flags,
						     uint64_t *async_transid,
						     struct btrfs_util_qgroup_inherit *qgroup_inherit);

/**
 * BTRFS_UTIL_DELETE_SUBVOLUME_RECURSIVE - Delete subvolumes beneath the given
 * subvolume before attempting to delete the given subvolume.
 *
 * If this flag is not used, deleting a subvolume with child subvolumes is an
 * error. Note that this is currently implemented in userspace non-atomically.
 * It requires appropriate privilege (CAP_SYS_ADMIN).
 */
#define BTRFS_UTIL_DELETE_SUBVOLUME_RECURSIVE (1 << 0)
#define BTRFS_UTIL_DELETE_SUBVOLUME_MASK ((1 << 1) - 1)

/**
 * btrfs_util_delete_subvolume() - Delete a subvolume or snapshot.
 * @path: Path of the subvolume to delete.
 * @flags: Bitmask of BTRFS_UTIL_DELETE_SUBVOLUME_* flags.
 *
 * This requires appropriate privilege (CAP_SYS_ADMIN), unless the filesystem is
 * mounted with 'user_subvol_rm_allowed'.
 *
 * NOTE: Since kernel 4.18 it is possible to delete an empty subvolume using
 * rmdir.  The sysfs file /sys/fs/btrfs/features/rmdir_subvol indicates whether
 * this feature is enabled or not.
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_delete_subvolume(const char *path, int flags);

/**
 * btrfs_util_delete_subvolume_fd() - Delete a subvolume or snapshot given its
 * parent and name.
 * @parent_fd: File descriptor of the subvolume's parent directory.
 * @name: Name of the subvolume.
 * @flags: See btrfs_util_delete_subvolume().
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_delete_subvolume_fd(int parent_fd,
						     const char *name,
						     int flags);

/**
 * btrfs_util_delete_subvolume_by_id_fd() - Delete a subvolume or snapshot using
 * subvolume id.
 * @fd: File descriptor of the subvolume's parent directory.
 * @subvolid: Subvolume id of the subvolume or snapshot to be deleted.
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_delete_subvolume_by_id_fd(int fd,
							   uint64_t subvolid);

struct btrfs_util_subvolume_iterator;

/**
 * BTRFS_UTIL_SUBVOLUME_ITERATOR_POST_ORDER - Iterate post-order. The default
 * behavior is pre-order, e.g., foo will be yielded before foo/bar. If this flag
 * is specified, foo/bar will be yielded before foo.
 */
#define BTRFS_UTIL_SUBVOLUME_ITERATOR_POST_ORDER (1 << 0)
#define BTRFS_UTIL_SUBVOLUME_ITERATOR_MASK ((1 << 1) - 1)

/**
 * btrfs_util_create_subvolume_iterator() - Create an iterator over subvolumes
 * in a Btrfs filesystem.
 * @path: Path in a Btrfs filesystem. This may be any path in the filesystem; it
 * does not have to refer to a subvolume unless @top is zero.
 * @top: List subvolumes beneath (but not including) the subvolume with this ID.
 * If zero is given, the subvolume ID of @path is used. To list all subvolumes,
 * pass %BTRFS_FS_TREE_OBJECTID (i.e., 5). The returned paths are relative to
 * the subvolume with this ID.
 * @flags: Bitmask of BTRFS_UTIL_SUBVOLUME_ITERATOR_* flags.
 * @ret: Returned iterator.
 *
 * Subvolume iterators require appropriate privilege (CAP_SYS_ADMIN) unless @top
 * is zero and the kernel supports BTRFS_IOC_GET_SUBVOL_ROOTREF and
 * BTRFS_IOC_INO_LOOKUP_USER (kernel >= 4.18). In this case, subvolumes which
 * cannot be accessed (e.g., due to permissions or other mounts) will be
 * skipped.
 *
 * The returned iterator must be freed with
 * btrfs_util_destroy_subvolume_iterator().
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_create_subvolume_iterator(const char *path,
							   uint64_t top,
							   int flags,
							   struct btrfs_util_subvolume_iterator **ret);

/**
 * btrfs_util_create_subvolume_iterator_fd() - See
 * btrfs_util_create_subvolume_iterator().
 */
enum btrfs_util_error btrfs_util_create_subvolume_iterator_fd(int fd,
							      uint64_t top,
							      int flags,
							      struct btrfs_util_subvolume_iterator **ret);

/**
 * btrfs_util_destroy_subvolume_iterator() - Destroy a subvolume iterator
 * previously created by btrfs_util_create_subvolume_iterator().
 * @iter: Iterator to destroy.
 */
void btrfs_util_destroy_subvolume_iterator(struct btrfs_util_subvolume_iterator *iter);

/**
 * btrfs_util_subvolume_iterator_fd() - Get the file descriptor associated with
 * a subvolume iterator.
 * @iter: Iterator to get.
 *
 * This can be used to get the file descriptor opened by
 * btrfs_util_create_subvolume_iterator() in order to use it for other
 * functions.
 *
 * Return: File descriptor.
 */
int btrfs_util_subvolume_iterator_fd(const struct btrfs_util_subvolume_iterator *iter);

/**
 * btrfs_util_subvolume_iterator_next() - Get the next subvolume from a
 * subvolume iterator.
 * @iter: Subvolume iterator.
 * @path_ret: Returned subvolume path, relative to the subvolume ID used to
 * create the iterator. May be %NULL.
 * Must be freed with free().
 * @id_ret: Returned subvolume ID. May be %NULL.
 *
 * This requires appropriate privilege (CAP_SYS_ADMIN) for kernels < 4.18. See
 * btrfs_util_create_subvolume_iterator().
 *
 * Return: %BTRFS_UTIL_OK on success, %BTRFS_UTIL_ERROR_STOP_ITERATION if there
 * are no more subvolumes, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_subvolume_iterator_next(struct btrfs_util_subvolume_iterator *iter,
							 char **path_ret,
							 uint64_t *id_ret);

/**
 * btrfs_util_subvolume_iterator_next_info() - Get information about the next
 * subvolume for a subvolume iterator.
 * @iter: Subvolume iterator.
 * @path_ret: See btrfs_util_subvolume_iterator_next().
 * @subvol: Returned subvolume information.
 *
 * This convenience function basically combines
 * btrfs_util_subvolume_iterator_next() and btrfs_util_subvolume_info().
 *
 * This requires appropriate privilege (CAP_SYS_ADMIN) for kernels < 4.18. See
 * btrfs_util_create_subvolume_iterator().
 *
 * Return: See btrfs_util_subvolume_iterator_next().
 */
enum btrfs_util_error btrfs_util_subvolume_iterator_next_info(struct btrfs_util_subvolume_iterator *iter,
							      char **path_ret,
							      struct btrfs_util_subvolume_info *subvol);

/**
 * btrfs_util_deleted_subvolumes() - Get a list of subvolume which have been
 * deleted but not yet cleaned up.
 * @path: Path on a Btrfs filesystem.
 * @ids: Returned array of subvolume IDs.
 * @n: Returned number of IDs in the @ids array.
 *
 * This requires appropriate privilege (CAP_SYS_ADMIN).
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_deleted_subvolumes(const char *path,
						    uint64_t **ids,
						    size_t *n);

/**
 * btrfs_util_deleted_subvolumes_fd() - See btrfs_util_deleted_subvolumes().
 */
enum btrfs_util_error btrfs_util_deleted_subvolumes_fd(int fd, uint64_t **ids,
						       size_t *n);

/**
 * btrfs_util_create_qgroup_inherit() - Create a qgroup inheritance specifier
 * for btrfs_util_create_subvolume() or btrfs_util_create_snapshot().
 * @flags: Must be zero.
 * @ret: Returned qgroup inheritance specifier.
 *
 * The returned structure must be freed with
 * btrfs_util_destroy_qgroup_inherit().
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_create_qgroup_inherit(int flags,
						       struct btrfs_util_qgroup_inherit **ret);

/**
 * btrfs_util_destroy_qgroup_inherit() - Destroy a qgroup inheritance specifier
 * previously created with btrfs_util_create_qgroup_inherit().
 * @inherit: Specifier to destroy.
 */
void btrfs_util_destroy_qgroup_inherit(struct btrfs_util_qgroup_inherit *inherit);

/**
 * btrfs_util_qgroup_inherit_add_group() - Add inheritance from a qgroup to a
 * qgroup inheritance specifier.
 * @inherit: Specifier to modify. May be reallocated.
 * @qgroupid: ID of qgroup to inherit from.
 *
 * Return: %BTRFS_UTIL_OK on success, non-zero error code on failure.
 */
enum btrfs_util_error btrfs_util_qgroup_inherit_add_group(struct btrfs_util_qgroup_inherit **inherit,
							  uint64_t qgroupid);

/**
 * btrfs_util_qgroup_inherit_get_groups() - Get the qgroups a qgroup inheritance
 * specifier contains.
 * @inherit: Qgroup inheritance specifier.
 * @groups: Returned array of qgroup IDs.
 * @n: Returned number of entries in the @groups array.
 */
void btrfs_util_qgroup_inherit_get_groups(const struct btrfs_util_qgroup_inherit *inherit,
					  const uint64_t **groups, size_t *n);

#ifdef __cplusplus
}
#endif

#endif /* BTRFS_UTIL_H */
