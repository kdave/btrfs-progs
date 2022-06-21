/*
 * Copyright (C) 2012 Alexander Block.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#ifndef __BTRFS_SEND_STREAM_H__
#define __BTRFS_SEND_STREAM_H__

#include "kerncompat.h"

struct btrfs_send_ops {
	int (*subvol)(const char *path, const u8 *uuid, u64 ctransid,
		      void *user);
	int (*snapshot)(const char *path, const u8 *uuid, u64 ctransid,
			const u8 *parent_uuid, u64 parent_ctransid,
			void *user);
	int (*mkfile)(const char *path, void *user);
	int (*mkdir)(const char *path, void *user);
	int (*mknod)(const char *path, u64 mode, u64 dev, void *user);
	int (*mkfifo)(const char *path, void *user);
	int (*mksock)(const char *path, void *user);
	int (*symlink)(const char *path, const char *lnk, void *user);
	int (*rename)(const char *from, const char *to, void *user);
	int (*link)(const char *path, const char *lnk, void *user);
	int (*unlink)(const char *path, void *user);
	int (*rmdir)(const char *path, void *user);
	int (*write)(const char *path, const void *data, u64 offset, u64 len,
		     void *user);
	int (*clone)(const char *path, u64 offset, u64 len,
		     const u8 *clone_uuid, u64 clone_ctransid,
		     const char *clone_path, u64 clone_offset,
		     void *user);
	int (*set_xattr)(const char *path, const char *name, const void *data,
			 int len, void *user);
	int (*remove_xattr)(const char *path, const char *name, void *user);
	int (*truncate)(const char *path, u64 size, void *user);
	int (*chmod)(const char *path, u64 mode, void *user);
	int (*chown)(const char *path, u64 uid, u64 gid, void *user);
	int (*utimes)(const char *path, struct timespec *at,
		      struct timespec *mt, struct timespec *ct,
		      void *user);
	int (*update_extent)(const char *path, u64 offset, u64 len, void *user);
	int (*encoded_write)(const char *path, const void *data, u64 offset,
			     u64 len, u64 unencoded_file_len, u64 unencoded_len,
			     u64 unencoded_offset, u32 compression,
			     u32 encryption, void *user);
	int (*fallocate)(const char *path, int mode, u64 offset, u64 len,
			 void *user);
	int (*fileattr)(const char *path, u64 attr, void *user);
};

int btrfs_read_and_process_send_stream(int fd,
				       struct btrfs_send_ops *ops, void *user,
				       int honor_end_cmd,
				       u64 max_errors);

#endif
