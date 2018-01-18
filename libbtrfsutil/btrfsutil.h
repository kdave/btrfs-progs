/*
 * Copyright (C) 2018 Facebook
 *
 * This file is part of libbtrfsutil.
 *
 * libbtrfsutil is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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
#define BTRFS_UTIL_VERSION_MINOR 0
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
 * This requires appropriate privilege (CAP_SYS_ADMIN).
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
