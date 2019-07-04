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

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include "stubs.h"

#include "btrfsutil_internal.h"

static bool is_root(void)
{
	return geteuid() == 0;
}

/*
 * This intentionally duplicates btrfs_util_is_subvolume_fd() instead of opening
 * a file descriptor and calling it, because fstat() and fstatfs() don't accept
 * file descriptors opened with O_PATH on old kernels (before v3.6 and before
 * v3.12, respectively), but stat() and statfs() can be called on a path that
 * the user doesn't have read or write permissions to.
 */
PUBLIC enum btrfs_util_error btrfs_util_is_subvolume(const char *path)
{
	struct statfs sfs;
	struct stat st;
	int ret;

	ret = statfs(path, &sfs);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_STATFS_FAILED;

	if (sfs.f_type != BTRFS_SUPER_MAGIC) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_NOT_BTRFS;
	}

	ret = stat(path, &st);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_STAT_FAILED;

	if (st.st_ino != BTRFS_FIRST_FREE_OBJECTID || !S_ISDIR(st.st_mode)) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_NOT_SUBVOLUME;
	}

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_is_subvolume_fd(int fd)
{
	struct statfs sfs;
	struct stat st;
	int ret;

	ret = fstatfs(fd, &sfs);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_STATFS_FAILED;

	if (sfs.f_type != BTRFS_SUPER_MAGIC) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_NOT_BTRFS;
	}

	ret = fstat(fd, &st);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_STAT_FAILED;

	if (st.st_ino != BTRFS_FIRST_FREE_OBJECTID || !S_ISDIR(st.st_mode)) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_NOT_SUBVOLUME;
	}

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_subvolume_id(const char *path,
						     uint64_t *id_ret)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_subvolume_id_fd(fd, id_ret);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_subvolume_id_fd(int fd,
							uint64_t *id_ret)
{
	struct btrfs_ioctl_ino_lookup_args args = {
		.treeid = 0,
		.objectid = BTRFS_FIRST_FREE_OBJECTID,
	};
	int ret;

	ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &args);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_INO_LOOKUP_FAILED;

	*id_ret = args.treeid;

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_subvolume_path(const char *path,
						       uint64_t id,
						       char **path_ret)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_subvolume_path_fd(fd, id, path_ret);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_subvolume_path_fd(int fd, uint64_t id,
							  char **path_ret)
{
	char *path, *p;
	size_t capacity = 4096;

	if (id == 0) {
		enum btrfs_util_error err;

		err = btrfs_util_is_subvolume_fd(fd);
		if (err)
			return err;

		err = btrfs_util_subvolume_id_fd(fd, &id);
		if (err)
			return err;
	}

	path = malloc(capacity);
	if (!path)
		return BTRFS_UTIL_ERROR_NO_MEMORY;
	p = path + capacity - 1;
	p[0] = '\0';

	while (id != BTRFS_FS_TREE_OBJECTID) {
		struct btrfs_ioctl_search_args search = {
			.key = {
				.tree_id = BTRFS_ROOT_TREE_OBJECTID,
				.min_objectid = id,
				.max_objectid = id,
				.min_type = BTRFS_ROOT_BACKREF_KEY,
				.max_type = BTRFS_ROOT_BACKREF_KEY,
				.min_offset = 0,
				.max_offset = UINT64_MAX,
				.min_transid = 0,
				.max_transid = UINT64_MAX,
				.nr_items = 1,
			},
		};
		struct btrfs_ioctl_ino_lookup_args lookup;
		const struct btrfs_ioctl_search_header *header;
		const struct btrfs_root_ref *ref;
		const char *name;
		uint16_t name_len;
		size_t lookup_len;
		size_t total_len;
		int ret;

		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &search);
		if (ret == -1) {
			free(path);
			return BTRFS_UTIL_ERROR_SEARCH_FAILED;
		}

		if (search.key.nr_items == 0) {
			free(path);
			errno = ENOENT;
			return BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND;
		}

		header = (struct btrfs_ioctl_search_header *)search.buf;
		ref = (struct btrfs_root_ref *)(header + 1);
		name = (char *)(ref + 1);
		name_len = le16_to_cpu(ref->name_len);

		id = header->offset;

		lookup.treeid = id;
		lookup.objectid = le64_to_cpu(ref->dirid);
		ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &lookup);
		if (ret == -1) {
			free(path);
			return BTRFS_UTIL_ERROR_SEARCH_FAILED;
		}
		lookup_len = strlen(lookup.name);

		total_len = name_len + lookup_len + (id != BTRFS_FS_TREE_OBJECTID);
		if (p - total_len < path) {
			char *new_path, *new_p;
			size_t new_capacity = capacity * 2;

			new_path = malloc(new_capacity);
			if (!new_path) {
				free(path);
				return BTRFS_UTIL_ERROR_NO_MEMORY;
			}
			new_p = new_path + new_capacity - (path + capacity - p);
			memcpy(new_p, p, path + capacity - p);
			free(path);
			path = new_path;
			p = new_p;
			capacity = new_capacity;
		}
		p -= name_len;
		memcpy(p, name, name_len);
		p -= lookup_len;
		memcpy(p, lookup.name, lookup_len);
		if (id != BTRFS_FS_TREE_OBJECTID)
			*--p = '/';
	}

	if (p != path)
		memmove(path, p, path + capacity - p);

	*path_ret = path;

	return BTRFS_UTIL_OK;
}

static void copy_timespec(struct timespec *timespec,
			  const struct btrfs_timespec *btrfs_timespec)
{
	timespec->tv_sec = le64_to_cpu(btrfs_timespec->sec);
	timespec->tv_nsec = le32_to_cpu(btrfs_timespec->nsec);
}

static void copy_root_item(struct btrfs_util_subvolume_info *subvol,
			   const struct btrfs_root_item *root)
{
	subvol->flags = le64_to_cpu(root->flags);
	memcpy(subvol->uuid, root->uuid, sizeof(subvol->uuid));
	memcpy(subvol->parent_uuid, root->parent_uuid,
	       sizeof(subvol->parent_uuid));
	memcpy(subvol->received_uuid, root->received_uuid,
	       sizeof(subvol->received_uuid));
	subvol->generation = le64_to_cpu(root->generation);
	subvol->ctransid = le64_to_cpu(root->ctransid);
	subvol->otransid = le64_to_cpu(root->otransid);
	subvol->stransid = le64_to_cpu(root->stransid);
	subvol->rtransid = le64_to_cpu(root->rtransid);
	copy_timespec(&subvol->ctime, &root->ctime);
	copy_timespec(&subvol->otime, &root->otime);
	copy_timespec(&subvol->stime, &root->stime);
	copy_timespec(&subvol->rtime, &root->rtime);
}

PUBLIC enum btrfs_util_error btrfs_util_subvolume_info(const char *path,
						       uint64_t id,
						       struct btrfs_util_subvolume_info *subvol)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_subvolume_info_fd(fd, id, subvol);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

static enum btrfs_util_error get_subvolume_info_privileged(int fd, uint64_t id,
							   struct btrfs_util_subvolume_info *subvol)
{
	struct btrfs_ioctl_search_args search = {
		.key = {
			.tree_id = BTRFS_ROOT_TREE_OBJECTID,
			.min_type = BTRFS_ROOT_ITEM_KEY,
			.max_type = BTRFS_ROOT_BACKREF_KEY,
			.min_offset = 0,
			.max_offset = UINT64_MAX,
			.min_transid = 0,
			.max_transid = UINT64_MAX,
			.nr_items = 0,
		},
	};
	size_t items_pos = 0, buf_off = 0;
	bool need_root_item = true, need_root_backref = true;
	int ret;

	search.key.min_objectid = search.key.max_objectid = id;

	if (subvol) {
		subvol->id = id;
		subvol->parent_id = 0;
		subvol->dir_id = 0;
		if (id == BTRFS_FS_TREE_OBJECTID)
			need_root_backref = false;
	} else {
		/*
		 * We only need the backref for filling in the subvolume info.
		 */
		need_root_backref = false;
	}

	/* Don't bother searching for the backref if we don't need it. */
	if (!need_root_backref)
		search.key.max_type = BTRFS_ROOT_ITEM_KEY;

	while (need_root_item || need_root_backref) {
		const struct btrfs_ioctl_search_header *header;

		if (items_pos >= search.key.nr_items) {
			search.key.nr_items = 4096;
			ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &search);
			if (ret == -1)
				return BTRFS_UTIL_ERROR_SEARCH_FAILED;
			items_pos = 0;
			buf_off = 0;

			if (search.key.nr_items == 0) {
				if (need_root_item) {
					errno = ENOENT;
					return BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND;
				} else {
					break;
				}
			}
		}

		header = (struct btrfs_ioctl_search_header *)(search.buf + buf_off);
		if (btrfs_search_header_type(header) == BTRFS_ROOT_ITEM_KEY) {
			if (subvol) {
				const struct btrfs_root_item *root;

				root = (const struct btrfs_root_item *)(header + 1);
				copy_root_item(subvol, root);
			}
			need_root_item = false;
			search.key.min_type = BTRFS_ROOT_BACKREF_KEY;
		} else if (btrfs_search_header_type(header) == BTRFS_ROOT_BACKREF_KEY) {
			if (subvol) {
				const struct btrfs_root_ref *ref;

				ref = (const struct btrfs_root_ref *)(header + 1);
				subvol->parent_id = btrfs_search_header_offset(header);
				subvol->dir_id = le64_to_cpu(ref->dirid);
			}
			need_root_backref = false;
			search.key.min_type = UINT32_MAX;
		}

		items_pos++;
		buf_off += sizeof(*header) + btrfs_search_header_len(header);
	}

	return BTRFS_UTIL_OK;
}

static enum btrfs_util_error get_subvolume_info_unprivileged(int fd,
							     struct btrfs_util_subvolume_info *subvol)
{
	struct btrfs_ioctl_get_subvol_info_args info;
	int ret;

	ret = ioctl(fd, BTRFS_IOC_GET_SUBVOL_INFO, &info);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_GET_SUBVOL_INFO_FAILED;

	subvol->id = info.treeid;
	subvol->parent_id = info.parent_id;
	subvol->dir_id = info.dirid;
	subvol->flags = info.flags;
	subvol->generation = info.generation;

	memcpy(subvol->uuid, info.uuid, sizeof(subvol->uuid));
	memcpy(subvol->parent_uuid, info.parent_uuid,
	       sizeof(subvol->parent_uuid));
	memcpy(subvol->received_uuid, info.received_uuid,
	       sizeof(subvol->received_uuid));

	subvol->ctransid = info.ctransid;
	subvol->otransid = info.otransid;
	subvol->stransid = info.stransid;
	subvol->rtransid = info.rtransid;

	subvol->ctime.tv_sec = info.ctime.sec;
	subvol->ctime.tv_nsec = info.ctime.nsec;
	subvol->otime.tv_sec = info.otime.sec;
	subvol->otime.tv_nsec = info.otime.nsec;
	subvol->stime.tv_sec = info.stime.sec;
	subvol->stime.tv_nsec = info.stime.nsec;
	subvol->rtime.tv_sec = info.rtime.sec;
	subvol->rtime.tv_nsec = info.rtime.nsec;

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_subvolume_info_fd(int fd, uint64_t id,
							  struct btrfs_util_subvolume_info *subvol)
{
	enum btrfs_util_error err;

	if (id == 0) {
		err = btrfs_util_is_subvolume_fd(fd);
		if (err)
			return err;

		if (!is_root())
			return get_subvolume_info_unprivileged(fd, subvol);

		err = btrfs_util_subvolume_id_fd(fd, &id);
		if (err)
			return err;
	}

	if ((id < BTRFS_FIRST_FREE_OBJECTID && id != BTRFS_FS_TREE_OBJECTID) ||
	    id > BTRFS_LAST_FREE_OBJECTID) {
		errno = ENOENT;
		return BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND;
	}

	return get_subvolume_info_privileged(fd, id, subvol);
}

PUBLIC enum btrfs_util_error btrfs_util_get_subvolume_read_only_fd(int fd,
								   bool *read_only_ret)
{
	uint64_t flags;
	int ret;

	ret = ioctl(fd, BTRFS_IOC_SUBVOL_GETFLAGS, &flags);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_SUBVOL_GETFLAGS_FAILED;

	*read_only_ret = flags & BTRFS_SUBVOL_RDONLY;
	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_get_subvolume_read_only(const char *path,
								bool *ret)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_get_subvolume_read_only_fd(fd, ret);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_set_subvolume_read_only(const char *path,
								bool read_only)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_set_subvolume_read_only_fd(fd, read_only);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_set_subvolume_read_only_fd(int fd,
								   bool read_only)
{
	uint64_t flags;
	int ret;

	ret = ioctl(fd, BTRFS_IOC_SUBVOL_GETFLAGS, &flags);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_SUBVOL_GETFLAGS_FAILED;

	if (read_only)
		flags |= BTRFS_SUBVOL_RDONLY;
	else
		flags &= ~BTRFS_SUBVOL_RDONLY;

	ret = ioctl(fd, BTRFS_IOC_SUBVOL_SETFLAGS, &flags);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_SUBVOL_SETFLAGS_FAILED;

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_get_default_subvolume(const char *path,
							      uint64_t *id_ret)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_get_default_subvolume_fd(fd, id_ret);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_get_default_subvolume_fd(int fd,
								 uint64_t *id_ret)
{
	struct btrfs_ioctl_search_args search = {
		.key = {
			.tree_id = BTRFS_ROOT_TREE_OBJECTID,
			.min_objectid = BTRFS_ROOT_TREE_DIR_OBJECTID,
			.max_objectid = BTRFS_ROOT_TREE_DIR_OBJECTID,
			.min_type = BTRFS_DIR_ITEM_KEY,
			.max_type = BTRFS_DIR_ITEM_KEY,
			.min_offset = 0,
			.max_offset = UINT64_MAX,
			.min_transid = 0,
			.max_transid = UINT64_MAX,
			.nr_items = 0,
		},
	};
	size_t items_pos = 0, buf_off = 0;
	int ret;

	for (;;) {
		const struct btrfs_ioctl_search_header *header;

		if (items_pos >= search.key.nr_items) {
			search.key.nr_items = 4096;
			ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &search);
			if (ret == -1)
				return BTRFS_UTIL_ERROR_SEARCH_FAILED;
			items_pos = 0;
			buf_off = 0;

			if (search.key.nr_items == 0) {
				errno = ENOENT;
				return BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND;
			}
		}

		header = (struct btrfs_ioctl_search_header *)(search.buf + buf_off);
		if (header->type == BTRFS_DIR_ITEM_KEY) {
			const struct btrfs_dir_item *dir;
			const char *name;
			uint16_t name_len;

			dir = (struct btrfs_dir_item *)(header + 1);
			name = (const char *)(dir + 1);
			name_len = le16_to_cpu(dir->name_len);
			if (strncmp(name, "default", name_len) == 0) {
				*id_ret = le64_to_cpu(dir->location.objectid);
				break;
			}
		}

		items_pos++;
		buf_off += sizeof(*header) + header->len;
		search.key.min_offset = header->offset + 1;
	}

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_set_default_subvolume(const char *path,
							      uint64_t id)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_set_default_subvolume_fd(fd, id);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_set_default_subvolume_fd(int fd,
								 uint64_t id)
{
	enum btrfs_util_error err;
	int ret;

	if (id == 0) {
		err = btrfs_util_is_subvolume_fd(fd);
		if (err)
			return err;

		err = btrfs_util_subvolume_id_fd(fd, &id);
		if (err)
			return err;
	}

	ret = ioctl(fd, BTRFS_IOC_DEFAULT_SUBVOL, &id);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_DEFAULT_SUBVOL_FAILED;

	return BTRFS_UTIL_OK;
}

static enum btrfs_util_error openat_parent_and_name(int dirfd, const char *path,
						    char *name, size_t name_len,
						    int *fd)
{
	char *tmp_path, *slash, *dirname, *basename;
	size_t len;

	/* Ignore trailing slashes. */
	len = strlen(path);
	while (len > 1 && path[len - 1] == '/')
		len--;

	tmp_path = malloc(len + 1);
	if (!tmp_path)
		return BTRFS_UTIL_ERROR_NO_MEMORY;
	memcpy(tmp_path, path, len);
	tmp_path[len] = '\0';

	slash = memrchr(tmp_path, '/', len);
	if (slash == tmp_path) {
		dirname = "/";
		basename = tmp_path + 1;
	} else if (slash) {
		*slash = '\0';
		dirname = tmp_path;
		basename = slash + 1;
	} else {
		dirname = ".";
		basename = tmp_path;
	}

	len = strlen(basename);
	if (len >= name_len) {
		free(tmp_path);
		errno = ENAMETOOLONG;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}
	memcpy(name, basename, len);
	name[len] = '\0';

	*fd = openat(dirfd, dirname, O_RDONLY | O_DIRECTORY);
	if (*fd == -1) {
		free(tmp_path);
		return BTRFS_UTIL_ERROR_OPEN_FAILED;
	}

	free(tmp_path);
	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_create_subvolume(const char *path,
							 int flags,
							 uint64_t *async_transid,
							 struct btrfs_util_qgroup_inherit *qgroup_inherit)
{
	char name[BTRFS_SUBVOL_NAME_MAX + 1];
	enum btrfs_util_error err;
	int parent_fd;

	err = openat_parent_and_name(AT_FDCWD, path, name, sizeof(name),
				     &parent_fd);
	if (err)
		return err;

	err = btrfs_util_create_subvolume_fd(parent_fd, name, flags,
					    async_transid, qgroup_inherit);
	SAVE_ERRNO_AND_CLOSE(parent_fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_create_subvolume_fd(int parent_fd,
							    const char *name,
							    int flags,
							    uint64_t *async_transid,
							    struct btrfs_util_qgroup_inherit *qgroup_inherit)
{
	struct btrfs_ioctl_vol_args_v2 args = {};
	size_t len;
	int ret;

	if (flags) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}

	if (async_transid)
		args.flags |= BTRFS_SUBVOL_CREATE_ASYNC;
	if (qgroup_inherit) {
		args.flags |= BTRFS_SUBVOL_QGROUP_INHERIT;
		args.qgroup_inherit = (struct btrfs_qgroup_inherit *)qgroup_inherit;
		args.size = (sizeof(*args.qgroup_inherit) +
			     args.qgroup_inherit->num_qgroups *
			     sizeof(args.qgroup_inherit->qgroups[0]));
	}

	len = strlen(name);
	if (len >= sizeof(args.name)) {
		errno = ENAMETOOLONG;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}
	memcpy(args.name, name, len);
	args.name[len] = '\0';

	ret = ioctl(parent_fd, BTRFS_IOC_SUBVOL_CREATE_V2, &args);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_SUBVOL_CREATE_FAILED;

	if (async_transid)
		*async_transid = args.transid;

	return BTRFS_UTIL_OK;
}

#define BTRFS_UTIL_SUBVOLUME_ITERATOR_CLOSE_FD (1 << 30)

struct search_stack_entry {
	union {
		/* Used for subvolume_iterator_next_tree_search(). */
		struct {
			struct btrfs_ioctl_search_args search;
			size_t buf_off;
		};
		/* Used for subvolume_iterator_next_unprivileged(). */
		struct {
			uint64_t id;
			struct btrfs_ioctl_get_subvol_rootref_args rootref_args;
		};
	};
	/* Used for both. */
	size_t items_pos;
	size_t path_len;
};

struct btrfs_util_subvolume_iterator {
	bool use_tree_search;
	int fd;
	/* cur_fd is only used for subvolume_iterator_next_unprivileged(). */
	int cur_fd;
	int flags;

	struct search_stack_entry *search_stack;
	size_t search_stack_len;
	size_t search_stack_capacity;

	char *cur_path;
	size_t cur_path_capacity;
};

static struct search_stack_entry *top_search_stack_entry(struct btrfs_util_subvolume_iterator *iter)
{
	return &iter->search_stack[iter->search_stack_len - 1];
}

/*
 * Check that a path that we opened is the subvolume which we expect. It may not
 * be if there is another filesystem mounted over a parent directory or the
 * subvolume itself.
 */
static enum btrfs_util_error check_expected_subvolume(int fd, int parent_fd,
						      uint64_t tree_id)
{
	struct btrfs_ioctl_fs_info_args parent_fs_info, fs_info;
	enum btrfs_util_error err;
	uint64_t id;
	int ret;

	/* Make sure it's a subvolume. */
	err = btrfs_util_is_subvolume_fd(fd);
	if (err == BTRFS_UTIL_ERROR_NOT_BTRFS ||
	    err == BTRFS_UTIL_ERROR_NOT_SUBVOLUME) {
		errno = ENOENT;
		return BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND;
	} else if (err) {
		return err;
	}

	/* Make sure it's on the same filesystem. */
	ret = ioctl(parent_fd, BTRFS_IOC_FS_INFO, &parent_fs_info);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_FS_INFO_FAILED;
	ret = ioctl(fd, BTRFS_IOC_FS_INFO, &fs_info);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_FS_INFO_FAILED;
	if (memcmp(parent_fs_info.fsid, fs_info.fsid, sizeof(fs_info.fsid)) != 0) {
		errno = ENOENT;
		return BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND;
	}

	/* Make sure it's the subvolume that we expected. */
	err = btrfs_util_subvolume_id_fd(fd, &id);
	if (err)
		return err;
	if (id != tree_id) {
		errno = ENOENT;
		return BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND;
	}

	return BTRFS_UTIL_OK;
}

static enum btrfs_util_error append_to_search_stack(struct btrfs_util_subvolume_iterator *iter,
						    uint64_t tree_id,
						    size_t path_len)
{
	struct search_stack_entry *entry;

	if (iter->search_stack_len >= iter->search_stack_capacity) {
		size_t new_capacity = iter->search_stack_capacity * 2;
		struct search_stack_entry *new_search_stack;

		new_search_stack = reallocarray(iter->search_stack,
						new_capacity,
						sizeof(*iter->search_stack));
		if (!new_search_stack)
			return BTRFS_UTIL_ERROR_NO_MEMORY;

		iter->search_stack_capacity = new_capacity;
		iter->search_stack = new_search_stack;
	}

	entry = &iter->search_stack[iter->search_stack_len];

	memset(entry, 0, sizeof(*entry));
	entry->path_len = path_len;
	if (iter->use_tree_search) {
		entry->search.key.tree_id = BTRFS_ROOT_TREE_OBJECTID;
		entry->search.key.min_objectid = tree_id;
		entry->search.key.max_objectid = tree_id;
		entry->search.key.min_type = BTRFS_ROOT_REF_KEY;
		entry->search.key.max_type = BTRFS_ROOT_REF_KEY;
		entry->search.key.min_offset = 0;
		entry->search.key.max_offset = UINT64_MAX;
		entry->search.key.min_transid = 0;
		entry->search.key.max_transid = UINT64_MAX;
		entry->search.key.nr_items = 0;
	} else {
		entry->id = tree_id;

		if (iter->search_stack_len) {
			struct search_stack_entry *top;
			enum btrfs_util_error err;
			char *path;
			int fd;

			top = top_search_stack_entry(iter);
			path = &iter->cur_path[top->path_len];
			if (*path == '/')
				path++;
			fd = openat(iter->cur_fd, path, O_RDONLY);
			if (fd == -1)
				return BTRFS_UTIL_ERROR_OPEN_FAILED;

			err = check_expected_subvolume(fd, iter->cur_fd,
						       tree_id);
			if (err) {
				close(fd);
				return err;
			}

			close(iter->cur_fd);
			iter->cur_fd = fd;
		}
	}

	iter->search_stack_len++;

	return BTRFS_UTIL_OK;
}

static enum btrfs_util_error pop_search_stack(struct btrfs_util_subvolume_iterator *iter)
{
	struct search_stack_entry *top, *parent;
	int fd, parent_fd;
	size_t i;

	if (iter->use_tree_search || iter->search_stack_len == 1) {
		iter->search_stack_len--;
		return BTRFS_UTIL_OK;
	}

	top = top_search_stack_entry(iter);
	iter->search_stack_len--;
	parent = top_search_stack_entry(iter);

	fd = iter->cur_fd;
	for (i = parent->path_len; i < top->path_len; i++) {
		if (i == 0 || iter->cur_path[i] == '/') {
			parent_fd = openat(fd, "..", O_RDONLY);
			if (fd != iter->cur_fd)
				SAVE_ERRNO_AND_CLOSE(fd);
			if (parent_fd == -1)
				return BTRFS_UTIL_ERROR_OPEN_FAILED;
			fd = parent_fd;
		}
	}
	if (iter->cur_fd != iter->fd)
		close(iter->cur_fd);
	iter->cur_fd = fd;

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_create_subvolume_iterator(const char *path,
								  uint64_t top,
								  int flags,
								  struct btrfs_util_subvolume_iterator **ret)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_create_subvolume_iterator_fd(fd, top, flags, ret);
	if (err)
		SAVE_ERRNO_AND_CLOSE(fd);
	else
		(*ret)->flags |= BTRFS_UTIL_SUBVOLUME_ITERATOR_CLOSE_FD;

	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_create_subvolume_iterator_fd(int fd,
								     uint64_t top,
								     int flags,
								     struct btrfs_util_subvolume_iterator **ret)
{
	struct btrfs_util_subvolume_iterator *iter;
	enum btrfs_util_error err;
	bool use_tree_search;

	if (flags & ~BTRFS_UTIL_SUBVOLUME_ITERATOR_MASK) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}

	use_tree_search = top != 0 || is_root();
	if (top == 0) {
		err = btrfs_util_is_subvolume_fd(fd);
		if (err)
			return err;

		err = btrfs_util_subvolume_id_fd(fd, &top);
		if (err)
			return err;
	}

	iter = malloc(sizeof(*iter));
	if (!iter)
		return BTRFS_UTIL_ERROR_NO_MEMORY;

	iter->fd = fd;
	iter->cur_fd = fd;
	iter->flags = flags;
	iter->use_tree_search = use_tree_search;

	iter->search_stack_len = 0;
	iter->search_stack_capacity = 4;
	iter->search_stack = malloc(sizeof(*iter->search_stack) *
				    iter->search_stack_capacity);
	if (!iter->search_stack) {
		err = BTRFS_UTIL_ERROR_NO_MEMORY;
		goto out_iter;
	}

	iter->cur_path_capacity = 256;
	iter->cur_path = malloc(iter->cur_path_capacity);
	if (!iter->cur_path) {
		err = BTRFS_UTIL_ERROR_NO_MEMORY;
		goto out_search_stack;
	}

	err = append_to_search_stack(iter, top, 0);
	if (err)
		goto out_cur_path;

	*ret = iter;

	return BTRFS_UTIL_OK;

out_cur_path:
	free(iter->cur_path);
out_search_stack:
	free(iter->search_stack);
out_iter:
	free(iter);
	return err;
}

static enum btrfs_util_error snapshot_subvolume_children(int fd, int parent_fd,
							 const char *name,
							 uint64_t *async_transid)
{
	struct btrfs_util_subvolume_iterator *iter;
	enum btrfs_util_error err;
	int dstfd;

	dstfd = openat(parent_fd, name, O_RDONLY);
	if (dstfd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_create_subvolume_iterator_fd(fd, 0, 0, &iter);
	if (err)
		goto out;

	for (;;) {
		char child_name[BTRFS_SUBVOL_NAME_MAX + 1];
		char *child_path;
		int child_fd, new_parent_fd;
		uint64_t tmp_transid;

		err = btrfs_util_subvolume_iterator_next(iter, &child_path,
							 NULL);
		if (err) {
			if (err == BTRFS_UTIL_ERROR_STOP_ITERATION)
				err = BTRFS_UTIL_OK;
			break;
		}

		/* Remove the placeholder directory. */
		if (unlinkat(dstfd, child_path, AT_REMOVEDIR) == -1) {
			free(child_path);
			err = BTRFS_UTIL_ERROR_RMDIR_FAILED;
			break;
		}

		child_fd = openat(fd, child_path, O_RDONLY);
		if (child_fd == -1) {
			free(child_path);
			err = BTRFS_UTIL_ERROR_OPEN_FAILED;
			break;
		}

		err = openat_parent_and_name(dstfd, child_path, child_name,
					     sizeof(child_name),
					     &new_parent_fd);
		free(child_path);
		if (err) {
			SAVE_ERRNO_AND_CLOSE(child_fd);
			break;
		}

		err = btrfs_util_create_snapshot_fd2(child_fd, new_parent_fd,
						     child_name, 0,
						     async_transid ? &tmp_transid : NULL,
						     NULL);
		SAVE_ERRNO_AND_CLOSE(child_fd);
		SAVE_ERRNO_AND_CLOSE(new_parent_fd);
		if (err)
			break;
		if (async_transid && tmp_transid > *async_transid)
			*async_transid = tmp_transid;
	}

	btrfs_util_destroy_subvolume_iterator(iter);
out:
	SAVE_ERRNO_AND_CLOSE(dstfd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_create_snapshot(const char *source,
							const char *path,
							int flags,
							uint64_t *async_transid,
							struct btrfs_util_qgroup_inherit *qgroup_inherit)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(source, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_create_snapshot_fd(fd, path, flags, async_transid,
					    qgroup_inherit);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_create_snapshot_fd(int fd,
							   const char *path,
							   int flags,
							   uint64_t *async_transid,
							   struct btrfs_util_qgroup_inherit *qgroup_inherit)
{
	char name[BTRFS_SUBVOL_NAME_MAX + 1];
	enum btrfs_util_error err;
	int parent_fd;

	err = openat_parent_and_name(AT_FDCWD, path, name, sizeof(name),
				     &parent_fd);
	if (err)
		return err;

	err = btrfs_util_create_snapshot_fd2(fd, parent_fd, name, flags,
					     async_transid, qgroup_inherit);
	SAVE_ERRNO_AND_CLOSE(parent_fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_create_snapshot_fd2(int fd,
							    int parent_fd,
							    const char *name,
							    int flags,
							    uint64_t *async_transid,
							    struct btrfs_util_qgroup_inherit *qgroup_inherit)
{
	struct btrfs_ioctl_vol_args_v2 args = {.fd = fd};
	enum btrfs_util_error err;
	size_t len;
	int ret;

	if ((flags & ~BTRFS_UTIL_CREATE_SNAPSHOT_MASK) ||
	    ((flags & BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY) &&
	     (flags & BTRFS_UTIL_CREATE_SNAPSHOT_RECURSIVE))) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}

	if (flags & BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY)
		args.flags |= BTRFS_SUBVOL_RDONLY;
	if (async_transid)
		args.flags |= BTRFS_SUBVOL_CREATE_ASYNC;
	if (qgroup_inherit) {
		args.flags |= BTRFS_SUBVOL_QGROUP_INHERIT;
		args.qgroup_inherit = (struct btrfs_qgroup_inherit *)qgroup_inherit;
		args.size = (sizeof(*args.qgroup_inherit) +
			     args.qgroup_inherit->num_qgroups *
			     sizeof(args.qgroup_inherit->qgroups[0]));
	}

	len = strlen(name);
	if (len >= sizeof(args.name)) {
		errno = ENAMETOOLONG;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}
	memcpy(args.name, name, len);
	args.name[len] = '\0';

	ret = ioctl(parent_fd, BTRFS_IOC_SNAP_CREATE_V2, &args);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_SUBVOL_CREATE_FAILED;

	if (async_transid)
		*async_transid = args.transid;

	if (flags & BTRFS_UTIL_CREATE_SNAPSHOT_RECURSIVE) {
		err = snapshot_subvolume_children(fd, parent_fd, name,
						  async_transid);
		if (err)
			return err;
	}

	return BTRFS_UTIL_OK;
}

static enum btrfs_util_error delete_subvolume_children(int parent_fd,
						       const char *name)
{
	struct btrfs_util_subvolume_iterator *iter;
	enum btrfs_util_error err;
	int fd;

	fd = openat(parent_fd, name, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_create_subvolume_iterator_fd(fd, 0,
						      BTRFS_UTIL_SUBVOLUME_ITERATOR_POST_ORDER,
						      &iter);
	if (err)
		goto out;

	for (;;) {
		char child_name[BTRFS_PATH_NAME_MAX + 1];
		char *child_path;
		int child_parent_fd;

		err = btrfs_util_subvolume_iterator_next(iter, &child_path,
							 NULL);
		if (err) {
			if (err == BTRFS_UTIL_ERROR_STOP_ITERATION)
				err = BTRFS_UTIL_OK;
			break;
		}

		err = openat_parent_and_name(fd, child_path, child_name,
					     sizeof(child_name),
					     &child_parent_fd);
		free(child_path);
		if (err)
			break;

		err = btrfs_util_delete_subvolume_fd(child_parent_fd,
						     child_name, 0);
		SAVE_ERRNO_AND_CLOSE(child_parent_fd);
		if (err)
			break;
	}

	btrfs_util_destroy_subvolume_iterator(iter);
out:
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_delete_subvolume(const char *path,
							 int flags)
{
	char name[BTRFS_PATH_NAME_MAX + 1];
	enum btrfs_util_error err;
	int parent_fd;

	err = openat_parent_and_name(AT_FDCWD, path, name, sizeof(name),
				     &parent_fd);
	if (err)
		return err;

	err = btrfs_util_delete_subvolume_fd(parent_fd, name, flags);
	SAVE_ERRNO_AND_CLOSE(parent_fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_delete_subvolume_fd(int parent_fd,
							    const char *name,
							    int flags)
{
	struct btrfs_ioctl_vol_args args = {};
	enum btrfs_util_error err;
	size_t len;
	int ret;

	if (flags & ~BTRFS_UTIL_DELETE_SUBVOLUME_MASK) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}

	if (flags & BTRFS_UTIL_DELETE_SUBVOLUME_RECURSIVE) {
		err = delete_subvolume_children(parent_fd, name);
		if (err)
			return err;
	}

	len = strlen(name);
	if (len >= sizeof(args.name)) {
		errno = ENAMETOOLONG;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}
	memcpy(args.name, name, len);
	args.name[len] = '\0';

	ret = ioctl(parent_fd, BTRFS_IOC_SNAP_DESTROY, &args);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_SNAP_DESTROY_FAILED;

	return BTRFS_UTIL_OK;
}

PUBLIC void btrfs_util_destroy_subvolume_iterator(struct btrfs_util_subvolume_iterator *iter)
{
	if (iter) {
		free(iter->cur_path);
		free(iter->search_stack);
		if (iter->cur_fd != iter->fd)
			SAVE_ERRNO_AND_CLOSE(iter->cur_fd);
		if (iter->flags & BTRFS_UTIL_SUBVOLUME_ITERATOR_CLOSE_FD)
			SAVE_ERRNO_AND_CLOSE(iter->fd);
		free(iter);
	}
}

PUBLIC int btrfs_util_subvolume_iterator_fd(const struct btrfs_util_subvolume_iterator *iter)
{
	return iter->fd;
}

static enum btrfs_util_error build_subvol_path(struct btrfs_util_subvolume_iterator *iter,
					       const char *name, size_t name_len,
					       const char *dir, size_t dir_len,
					       size_t *path_len_ret)
{
	struct search_stack_entry *top = top_search_stack_entry(iter);
	size_t path_len;
	char *p;

	path_len = top->path_len;
	/*
	 * We need a joining slash if we have a current path and a subdirectory.
	 */
	if (top->path_len && dir_len)
		path_len++;
	path_len += dir_len;
	/*
	 * We need another joining slash if we have a current path and a name,
	 * but not if we have a subdirectory, because the lookup ioctl includes
	 * a trailing slash.
	 */
	if (top->path_len && !dir_len && name_len)
		path_len++;
	path_len += name_len;

	/* We need one extra character for the NUL terminator. */
	if (path_len + 1 > iter->cur_path_capacity) {
		char *tmp = realloc(iter->cur_path, path_len + 1);

		if (!tmp)
			return BTRFS_UTIL_ERROR_NO_MEMORY;
		iter->cur_path = tmp;
		iter->cur_path_capacity = path_len + 1;
	}

	p = iter->cur_path + top->path_len;
	if (top->path_len && dir_len)
		*p++ = '/';
	memcpy(p, dir, dir_len);
	p += dir_len;
	if (top->path_len && !dir_len && name_len)
		*p++ = '/';
	memcpy(p, name, name_len);
	p += name_len;
	*p = '\0';

	*path_len_ret = path_len;

	return BTRFS_UTIL_OK;
}

static enum btrfs_util_error build_subvol_path_privileged(struct btrfs_util_subvolume_iterator *iter,
							  const struct btrfs_ioctl_search_header *header,
							  const struct btrfs_root_ref *ref,
							  const char *name,
							  size_t *path_len_ret)
{
	struct btrfs_ioctl_ino_lookup_args lookup = {
		.treeid = btrfs_search_header_objectid(header),
		.objectid = le64_to_cpu(ref->dirid),
	};
	int ret;

	ret = ioctl(iter->fd, BTRFS_IOC_INO_LOOKUP, &lookup);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_INO_LOOKUP_FAILED;

	return build_subvol_path(iter, name, le16_to_cpu(ref->name_len),
				 lookup.name, strlen(lookup.name),
				 path_len_ret);
}

static enum btrfs_util_error build_subvol_path_unprivileged(struct btrfs_util_subvolume_iterator *iter,
							    uint64_t treeid,
							    uint64_t dirid,
							    size_t *path_len_ret)
{
	struct btrfs_ioctl_ino_lookup_user_args args = {
		.treeid = treeid,
		.dirid = dirid,
	};
	int ret;

	ret = ioctl(iter->cur_fd, BTRFS_IOC_INO_LOOKUP_USER, &args);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_INO_LOOKUP_USER_FAILED;

	return build_subvol_path(iter, args.name, strlen(args.name),
				 args.path, strlen(args.path), path_len_ret);
}

static enum btrfs_util_error subvolume_iterator_next_tree_search(struct btrfs_util_subvolume_iterator *iter,
								 char **path_ret,
								 uint64_t *id_ret)
{
	struct search_stack_entry *top;
	const struct btrfs_ioctl_search_header *header;
	const struct btrfs_root_ref *ref;
	const char *name;
	enum btrfs_util_error err;
	size_t path_len;
	int ret;

	for (;;) {
		for (;;) {
			if (iter->search_stack_len == 0)
				return BTRFS_UTIL_ERROR_STOP_ITERATION;

			top = top_search_stack_entry(iter);
			if (top->items_pos < top->search.key.nr_items) {
				break;
			} else {
				top->search.key.nr_items = 4096;
				ret = ioctl(iter->fd, BTRFS_IOC_TREE_SEARCH, &top->search);
				if (ret == -1)
					return BTRFS_UTIL_ERROR_SEARCH_FAILED;
				top->items_pos = 0;
				top->buf_off = 0;

				if (top->search.key.nr_items == 0) {
					/*
					 * This never fails for use_tree_search.
					 */
					pop_search_stack(iter);
					if ((iter->flags & BTRFS_UTIL_SUBVOLUME_ITERATOR_POST_ORDER) &&
					    iter->search_stack_len)
						goto out;
				}
			}
		}

		header = (struct btrfs_ioctl_search_header *)(top->search.buf + top->buf_off);

		top->items_pos++;
		top->buf_off += sizeof(*header) + btrfs_search_header_len(header);
		top->search.key.min_offset = btrfs_search_header_offset(header) + 1;

		/* This shouldn't happen, but handle it just in case. */
		if (btrfs_search_header_type(header) != BTRFS_ROOT_REF_KEY)
			continue;

		ref = (struct btrfs_root_ref *)(header + 1);
		name = (const char *)(ref + 1);
		err = build_subvol_path_privileged(iter, header, ref, name,
						   &path_len);
		if (err)
			return err;

		err = append_to_search_stack(iter,
				btrfs_search_header_offset(header), path_len);
		if (err)
			return err;

		if (!(iter->flags & BTRFS_UTIL_SUBVOLUME_ITERATOR_POST_ORDER)) {
			top = top_search_stack_entry(iter);
			goto out;
		}
	}

out:
	if (path_ret) {
		*path_ret = malloc(top->path_len + 1);
		if (!*path_ret)
			return BTRFS_UTIL_ERROR_NO_MEMORY;
		memcpy(*path_ret, iter->cur_path, top->path_len);
		(*path_ret)[top->path_len] = '\0';
	}
	if (id_ret)
		*id_ret = top->search.key.min_objectid;
	return BTRFS_UTIL_OK;
}

static enum btrfs_util_error subvolume_iterator_next_unprivileged(struct btrfs_util_subvolume_iterator *iter,
								  char **path_ret,
								  uint64_t *id_ret)
{
	struct search_stack_entry *top;
	uint64_t treeid, dirid;
	enum btrfs_util_error err;
	size_t path_len;
	int ret;

	for (;;) {
		for (;;) {
			if (iter->search_stack_len == 0)
				return BTRFS_UTIL_ERROR_STOP_ITERATION;

			top = top_search_stack_entry(iter);
			if (top->items_pos < top->rootref_args.num_items) {
				break;
			} else {
				ret = ioctl(iter->cur_fd,
					    BTRFS_IOC_GET_SUBVOL_ROOTREF,
					    &top->rootref_args);
				if (ret == -1 && errno != EOVERFLOW)
					return BTRFS_UTIL_ERROR_GET_SUBVOL_ROOTREF_FAILED;
				top->items_pos = 0;

				if (top->rootref_args.num_items == 0) {
					err = pop_search_stack(iter);
					if (err)
						return err;
					if ((iter->flags & BTRFS_UTIL_SUBVOLUME_ITERATOR_POST_ORDER) &&
					    iter->search_stack_len)
						goto out;
				}
			}
		}

		treeid = top->rootref_args.rootref[top->items_pos].treeid;
		dirid = top->rootref_args.rootref[top->items_pos].dirid;
		top->items_pos++;
		err = build_subvol_path_unprivileged(iter, treeid, dirid,
						     &path_len);
		if (err) {
			/* Skip the subvolume if we can't access it. */
			if (errno == EACCES)
				continue;
			return err;
		}

		err = append_to_search_stack(iter, treeid, path_len);
		if (err) {
			/*
			 * Skip the subvolume if it does not exist (which can
			 * happen if there is another filesystem mounted over a
			 * parent directory) or we don't have permission to
			 * access it.
			 */
			if (errno == ENOENT || errno == EACCES)
				continue;
			return err;
		}

		if (!(iter->flags & BTRFS_UTIL_SUBVOLUME_ITERATOR_POST_ORDER)) {
			top = top_search_stack_entry(iter);
			goto out;
		}
	}

out:
	if (path_ret) {
		*path_ret = malloc(top->path_len + 1);
		if (!*path_ret)
			return BTRFS_UTIL_ERROR_NO_MEMORY;
		memcpy(*path_ret, iter->cur_path, top->path_len);
		(*path_ret)[top->path_len] = '\0';
	}
	if (id_ret)
		*id_ret = top->id;
	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_subvolume_iterator_next(struct btrfs_util_subvolume_iterator *iter,
								char **path_ret,
								uint64_t *id_ret)
{
	if (iter->use_tree_search) {
		return subvolume_iterator_next_tree_search(iter, path_ret,
							   id_ret);
	} else {
		return subvolume_iterator_next_unprivileged(iter, path_ret,
							    id_ret);
	}
}

PUBLIC enum btrfs_util_error btrfs_util_subvolume_iterator_next_info(struct btrfs_util_subvolume_iterator *iter,
								     char **path_ret,
								     struct btrfs_util_subvolume_info *subvol)
{
	enum btrfs_util_error err;
	uint64_t id;

	err = btrfs_util_subvolume_iterator_next(iter, path_ret, &id);
	if (err)
		return err;

	if (iter->use_tree_search)
		return btrfs_util_subvolume_info_fd(iter->fd, id, subvol);
	else
		return btrfs_util_subvolume_info_fd(iter->cur_fd, 0, subvol);
}

PUBLIC enum btrfs_util_error btrfs_util_deleted_subvolumes(const char *path,
							   uint64_t **ids,
							   size_t *n)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_deleted_subvolumes_fd(fd, ids, n);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_deleted_subvolumes_fd(int fd,
							      uint64_t **ids,
							      size_t *n)
{
	size_t capacity = 0;
	struct btrfs_ioctl_search_args search = {
		.key = {
			.tree_id = BTRFS_ROOT_TREE_OBJECTID,
			.min_objectid = BTRFS_ORPHAN_OBJECTID,
			.max_objectid = BTRFS_ORPHAN_OBJECTID,
			.min_type = BTRFS_ORPHAN_ITEM_KEY,
			.max_type = BTRFS_ORPHAN_ITEM_KEY,
			.min_offset = 0,
			.max_offset = UINT64_MAX,
			.min_transid = 0,
			.max_transid = UINT64_MAX,
			.nr_items = 0,
		},
	};
	enum btrfs_util_error err;
	size_t items_pos = 0, buf_off = 0;
	int ret;

	*ids = NULL;
	*n = 0;
	for (;;) {
		const struct btrfs_ioctl_search_header *header;

		if (items_pos >= search.key.nr_items) {
			search.key.nr_items = 4096;
			ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &search);
			if (ret == -1) {
				err = BTRFS_UTIL_ERROR_SEARCH_FAILED;
				goto out;
			}
			items_pos = 0;
			buf_off = 0;

			if (search.key.nr_items == 0)
				break;
		}

		header = (struct btrfs_ioctl_search_header *)(search.buf + buf_off);

		/*
		 * The orphan item might be for a free space cache inode, so
		 * check if there's a matching root item.
		 */
		err = btrfs_util_subvolume_info_fd(fd, header->offset, NULL);
		if (!err) {
			if (*n >= capacity) {
				size_t new_capacity;
				uint64_t *new_ids;

				new_capacity = capacity ? capacity * 2 : 1;
				new_ids = reallocarray(*ids, new_capacity,
						       sizeof(**ids));
				if (!new_ids) {
					err = BTRFS_UTIL_ERROR_NO_MEMORY;
					goto out;
				}

				*ids = new_ids;
				capacity = new_capacity;
			}
			(*ids)[(*n)++] = header->offset;
		} else if (err != BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND) {
			goto out;
		}

		items_pos++;
		buf_off += sizeof(*header) + header->len;
		search.key.min_offset = header->offset + 1;
	}

	err = BTRFS_UTIL_OK;
out:
	if (err) {
		free(*ids);
		*ids = NULL;
		*n = 0;
	}
	return err;
}
