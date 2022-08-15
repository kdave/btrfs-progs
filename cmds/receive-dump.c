/*
 * Copyright (C) 2016 Fujitsu.  All rights reserved.
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

#include "kerncompat.h"
#include <limits.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <uuid/uuid.h>
#include "common/defs.h"
#include "common/messages.h"
#include "common/send-stream.h"
#include "common/path-utils.h"
#include "cmds/receive-dump.h"

#define PATH_CAT_OR_RET(function_name, outpath, path1, path2, ret)	\
({									\
	ret = path_cat_out(outpath, path1, path2);			\
	if (ret < 0) {							\
		error("%s: path invalid: %s\n", function_name, path2);	\
		return ret;						\
	}								\
})

/*
 * Print path and escape characters (in a C way) that could break the line.
 * Returns the length of the escaped characters. Unprintable characters are
 * escaped as octals.
 */
static int print_path_escaped(const char *path)
{
	size_t i;
	size_t path_len = strlen(path);
	int len = 0;

	for (i = 0; i < path_len; i++) {
		char c = path[i];

		len++;
		switch (c) {
		case '\a': putchar('\\'); putchar('a'); len++; break;
		case '\b': putchar('\\'); putchar('b'); len++; break;
		case '\e': putchar('\\'); putchar('e'); len++; break;
		case '\f': putchar('\\'); putchar('f'); len++; break;
		case '\n': putchar('\\'); putchar('n'); len++; break;
		case '\r': putchar('\\'); putchar('r'); len++; break;
		case '\t': putchar('\\'); putchar('t'); len++; break;
		case '\v': putchar('\\'); putchar('v'); len++; break;
		case ' ':  putchar('\\'); putchar(' '); len++; break;
		case '\\': putchar('\\'); putchar('\\'); len++; break;
		default:
			  if (!isprint(c)) {
				  printf("\\%c%c%c",
						  '0' + ((c & 0300) >> 6),
						  '0' + ((c & 070) >> 3),
						  '0' + (c & 07));
				  len += 3;
			  } else {
				  putchar(c);
			  }
		}
	}
	return len;
}

enum print_mode {
	PRINT_DUMP_NORMAL,
	PRINT_DUMP_SUBVOLUME,
	PRINT_DUMP_NONEWLINE
};

/*
 * Underlying PRINT_DUMP, the only difference is how we handle
 * the full path.
 */
__attribute__ ((format (printf, 5, 6)))
static int __print_dump(enum print_mode mode, void *user, const char *path,
			const char *title, const char *fmt, ...)
{
	struct btrfs_dump_send_args *r = user;
	char full_path[PATH_MAX] = {0};
	char *out_path;
	va_list args;
	int ret;

	if (mode == PRINT_DUMP_SUBVOLUME) {
		PATH_CAT_OR_RET(title, r->full_subvol_path, r->root_path, path, ret);
		out_path = r->full_subvol_path;
	} else {
		PATH_CAT_OR_RET(title, full_path, r->full_subvol_path, path, ret);
		out_path = full_path;
	}

	/* Unified header */
	printf("%-16s", title);
	ret = print_path_escaped(out_path);
	if (!fmt) {
		putchar('\n');
		return 0;
	}
	/* Short paths are aligned to 32 chars; longer paths get a single space */
	do {
		putchar(' ');
	} while (++ret < 32);
	va_start(args, fmt);
	/* Operation specified ones */
	vprintf(fmt, args);
	va_end(args);
	if (mode != PRINT_DUMP_NONEWLINE)
		putchar('\n');
	return 0;
}

/* For subvolume/snapshot operation only */
#define PRINT_DUMP_SUBVOL(user, path, title, fmt, ...) \
	__print_dump(PRINT_DUMP_SUBVOLUME, user, path, title, fmt, ##__VA_ARGS__)

/* For other operations */
#define PRINT_DUMP(user, path, title, fmt, ...) \
	__print_dump(PRINT_DUMP_NORMAL, user, path, title, fmt, ##__VA_ARGS__)

/* For commands that may want to format parts of the line by themselves */
#define PRINT_DUMP_NO_NEWLINE(user, path, title, fmt, ...) \
	__print_dump(PRINT_DUMP_NONEWLINE, user, path, title, fmt, ##__VA_ARGS__)

static int print_subvol(const char *path, const u8 *uuid, u64 ctransid,
			void *user)
{
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];

	uuid_unparse(uuid, uuid_str);

	return PRINT_DUMP_SUBVOL(user, path, "subvol", "uuid=%s transid=%llu",
				 uuid_str, ctransid);
}

static int print_snapshot(const char *path, const u8 *uuid, u64 ctransid,
			  const u8 *parent_uuid, u64 parent_ctransid,
			  void *user)
{
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];
	char parent_uuid_str[BTRFS_UUID_UNPARSED_SIZE];
	int ret;

	uuid_unparse(uuid, uuid_str);
	uuid_unparse(parent_uuid, parent_uuid_str);

	ret = PRINT_DUMP_SUBVOL(user, path, "snapshot",
		"uuid=%s transid=%llu parent_uuid=%s parent_transid=%llu",
				uuid_str, ctransid, parent_uuid_str,
				parent_ctransid);
	return ret;
}

static int print_mkfile(const char *path, void *user)
{
	return PRINT_DUMP(user, path, "mkfile", NULL);
}

static int print_mkdir(const char *path, void *user)
{
	return PRINT_DUMP(user, path, "mkdir", NULL);
}

static int print_mknod(const char *path, u64 mode, u64 dev, void *user)
{
	return PRINT_DUMP(user, path, "mknod", "mode=%llo dev=0x%llx", mode,
			  dev);
}

static int print_mkfifo(const char *path, void *user)
{
	return PRINT_DUMP(user, path, "mkfifo", NULL);
}

static int print_mksock(const char *path, void *user)
{
	return PRINT_DUMP(user, path, "mksock", NULL);
}

static int print_symlink(const char *path, const char *lnk, void *user)
{
	PRINT_DUMP_NO_NEWLINE(user, path, "symlink", "dest=");
	print_path_escaped(lnk);
	putchar('\n');
	return 0;
}

static int print_rename(const char *from, const char *to, void *user)
{
	struct btrfs_dump_send_args *r = user;
	char full_to[PATH_MAX];
	int ret;

	PATH_CAT_OR_RET("rename", full_to, r->full_subvol_path, to, ret);
	PRINT_DUMP_NO_NEWLINE(user, from, "rename", "dest=");
	print_path_escaped(full_to);
	putchar('\n');
	return 0;
}

static int print_link(const char *path, const char *lnk, void *user)
{
	PRINT_DUMP_NO_NEWLINE(user, path, "link", "dest=");
	print_path_escaped(lnk);
	putchar('\n');
	return 0;
}

static int print_unlink(const char *path, void *user)
{
	return PRINT_DUMP(user, path, "unlink", NULL);
}

static int print_rmdir(const char *path, void *user)
{
	return PRINT_DUMP(user, path, "rmdir", NULL);
}

static int print_write(const char *path, const void *data, u64 offset,
		       u64 len, void *user)
{
	return PRINT_DUMP(user, path, "write", "offset=%llu len=%llu",
			  offset, len);
}

static int print_clone(const char *path, u64 offset, u64 len,
		       const u8 *clone_uuid, u64 clone_ctransid,
		       const char *clone_path, u64 clone_offset,
		       void *user)
{
	struct btrfs_dump_send_args *r = user;
	char full_path[PATH_MAX];
	int ret;

	PATH_CAT_OR_RET("clone", full_path, r->full_subvol_path, clone_path,
			ret);
	return PRINT_DUMP(user, path, "clone",
			  "offset=%llu len=%llu from=%s clone_offset=%llu",
			  offset, len, full_path, clone_offset);
}

static int print_set_xattr(const char *path, const char *name,
			   const void *data, int len, void *user)
{
	return PRINT_DUMP(user, path, "set_xattr", "name=%s data=%.*s len=%d",
			  name, len, (char *)data, len);
}

static int print_remove_xattr(const char *path, const char *name, void *user)
{

	return PRINT_DUMP(user, path, "remove_xattr", "name=%s", name);
}

static int print_truncate(const char *path, u64 size, void *user)
{
	return PRINT_DUMP(user, path, "truncate", "size=%llu", size);
}

static int print_chmod(const char *path, u64 mode, void *user)
{
	return PRINT_DUMP(user, path, "chmod", "mode=%llo", mode);
}

static int print_chown(const char *path, u64 uid, u64 gid, void *user)
{
	return PRINT_DUMP(user, path, "chown", "gid=%llu uid=%llu", gid, uid);
}

static int sprintf_timespec(struct timespec *ts, char *dest, int max_size)
{
	struct tm tm;
	int ret;

	if (!localtime_r(&ts->tv_sec, &tm)) {
		error("failed to convert time %lld.%.9ld to local time",
		      (long long)ts->tv_sec, ts->tv_nsec);
		return -EINVAL;
	}
	ret = strftime(dest, max_size, "%FT%T%z", &tm);
	if (ret == 0) {
		error(
		"time %lld.%ld is too long to convert into readable string",
		      (long long)ts->tv_sec, ts->tv_nsec);
		return -EINVAL;
	}
	return 0;
}

#define TIME_STRING_MAX	64
static int print_utimes(const char *path, struct timespec *at,
			struct timespec *mt, struct timespec *ct,
			void *user)
{
	char at_str[TIME_STRING_MAX];
	char mt_str[TIME_STRING_MAX];
	char ct_str[TIME_STRING_MAX];

	if (sprintf_timespec(at, at_str, TIME_STRING_MAX - 1) < 0 ||
	    sprintf_timespec(mt, mt_str, TIME_STRING_MAX - 1) < 0 ||
	    sprintf_timespec(ct, ct_str, TIME_STRING_MAX - 1) < 0)
		return -EINVAL;
	return PRINT_DUMP(user, path, "utimes", "atime=%s mtime=%s ctime=%s",
			  at_str, mt_str, ct_str);
}

static int print_update_extent(const char *path, u64 offset, u64 len,
			       void *user)
{
	return PRINT_DUMP(user, path, "update_extent", "offset=%llu len=%llu",
			  offset, len);
}

static int print_encoded_write(const char *path, const void *data, u64 offset,
			       u64 len, u64 unencoded_file_len,
			       u64 unencoded_len, u64 unencoded_offset,
			       u32 compression, u32 encryption, void *user)
{
	return PRINT_DUMP(user, path, "encoded_write",
			  "offset=%llu len=%llu, unencoded_file_len=%llu, "
			  "unencoded_len=%llu, unencoded_offset=%llu, "
			  "compression=%u, encryption=%u",
			  offset, len, unencoded_file_len, unencoded_len,
			  unencoded_offset, compression, encryption);
}

static int print_fallocate(const char *path, int mode, u64 offset, u64 len,
			   void *user)
{
	return PRINT_DUMP(user, path, "fallocate",
			  "mode=%d offset=%llu len=%llu",
			  mode, offset, len);
}

static int print_fileattr(const char *path, u64 attr, void *user)
{
	return PRINT_DUMP(user, path, "fileattr", "fileattr=0x%llu", attr);
}

static int print_enable_verity (const char *path, u8 algorithm, u32 block_size,
				int salt_len, char *salt,
				int sig_len, char *sig, void *user)
{
	return PRINT_DUMP(user, path, "enable_verity",
			  "algorithm=%u block_size=%u salt_len=%d sig_len=%d",
			  algorithm, block_size, salt_len, sig_len);
}

struct btrfs_send_ops btrfs_print_send_ops = {
	.subvol = print_subvol,
	.snapshot = print_snapshot,
	.mkfile = print_mkfile,
	.mkdir = print_mkdir,
	.mknod = print_mknod,
	.mkfifo = print_mkfifo,
	.mksock = print_mksock,
	.symlink = print_symlink,
	.rename = print_rename,
	.link = print_link,
	.unlink = print_unlink,
	.rmdir = print_rmdir,
	.write = print_write,
	.clone = print_clone,
	.set_xattr = print_set_xattr,
	.remove_xattr = print_remove_xattr,
	.truncate = print_truncate,
	.chmod = print_chmod,
	.chown = print_chown,
	.utimes = print_utimes,
	.update_extent = print_update_extent,
	.encoded_write = print_encoded_write,
	.fallocate = print_fallocate,
	.fileattr = print_fileattr,
	.enable_verity = print_enable_verity,
};
