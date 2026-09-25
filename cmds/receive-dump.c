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
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <uuid/uuid.h>
#include "common/defs.h"
#include "common/format-output.h"
#include "common/messages.h"
#include "common/send-stream.h"
#include "common/path-utils.h"
#include "common/string-utils.h"
#include "common/utils.h"
#include "cmds/commands.h"
#include "cmds/receive-dump.h"

#define PATH_CAT_OR_RET(function_name, outpath, path1, path2, ret)	\
({									\
	ret = path_cat_out(outpath, path1, path2);			\
	if (ret < 0) {							\
		error("%s: path invalid: %s", function_name, path2);	\
		return ret;						\
	}								\
})

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
	ret = string_print_escape_special(out_path);
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

/*
 * Json output
 *
 * The whole stream is printed as one json document with an array of objects,
 * one object per operation. Operations are printed as they are read, no data
 * are accumulated. The document is started and finished by
 * btrfs_dump_json_start() and btrfs_dump_json_end(), i.e. the text output is
 * not affected at all.
 *
 * The json printer does not use the text formatting from __print_dump(),
 * paths and other values are printed as native json types, the rowspec below
 * is only used to define the value types and the key names.
 */
static struct format_ctx dump_json_ctx;

/*
 * The keys and types are only used for the json output, the text output is
 * printed directly and does not use the rowspec.
 */
static const struct rowspec dump_json_rowspec[] = {
	{ .key = "op", .fmt = "%s", .out_json = "op" },
	{ .key = "path", .fmt = "str", .out_json = "path" },
	{ .key = "uuid", .fmt = "uuid", .out_json = "uuid" },
	{ .key = "ctransid", .fmt = "%llu", .out_json = "ctransid" },
	{ .key = "parent_uuid", .fmt = "uuid", .out_json = "parent_uuid" },
	{ .key = "parent_ctransid", .fmt = "%llu", .out_json = "parent_ctransid" },
	{ .key = "mode", .fmt = "%llu", .out_json = "mode" },
	{ .key = "dev", .fmt = "%llu", .out_json = "dev" },
	{ .key = "dest", .fmt = "str", .out_json = "dest" },
	{ .key = "offset", .fmt = "%llu", .out_json = "offset" },
	{ .key = "length", .fmt = "%llu", .out_json = "length" },
	{ .key = "from", .fmt = "str", .out_json = "from" },
	{ .key = "clone_uuid", .fmt = "uuid", .out_json = "clone_uuid" },
	{ .key = "clone_ctransid", .fmt = "%llu", .out_json = "clone_ctransid" },
	{ .key = "clone_offset", .fmt = "%llu", .out_json = "clone_offset" },
	{ .key = "name", .fmt = "str", .out_json = "name" },
	/* Xattr values can be arbitrary binary data */
	{ .key = "value", .fmt = "str-len", .out_json = "value" },
	{ .key = "size", .fmt = "%llu", .out_json = "size" },
	{ .key = "uid", .fmt = "%llu", .out_json = "uid" },
	{ .key = "gid", .fmt = "%llu", .out_json = "gid" },
	{ .key = "atime", .fmt = "str", .out_json = "atime" },
	{ .key = "mtime", .fmt = "str", .out_json = "mtime" },
	{ .key = "ctime", .fmt = "str", .out_json = "ctime" },
	{ .key = "unencoded_file_len", .fmt = "%llu", .out_json = "unencoded_file_len" },
	{ .key = "unencoded_len", .fmt = "%llu", .out_json = "unencoded_len" },
	{ .key = "unencoded_offset", .fmt = "%llu", .out_json = "unencoded_offset" },
	{ .key = "compression", .fmt = "%llu", .out_json = "compression" },
	{ .key = "encryption", .fmt = "%llu", .out_json = "encryption" },
	{ .key = "fileattr", .fmt = "%llu", .out_json = "fileattr" },
	{ .key = "algorithm", .fmt = "%llu", .out_json = "algorithm" },
	{ .key = "block_size", .fmt = "%llu", .out_json = "block_size" },
	{ .key = "salt_len", .fmt = "%llu", .out_json = "salt_len" },
	{ .key = "sig_len", .fmt = "%llu", .out_json = "sig_len" },
	ROWSPEC_END
};

void btrfs_dump_json_start(void)
{
	fmt_start(&dump_json_ctx, dump_json_rowspec, 0, 0);
	fmt_print_start_group(&dump_json_ctx, "operations", JSON_TYPE_ARRAY);
}

void btrfs_dump_json_end(void)
{
	fmt_print_end_group(&dump_json_ctx, "operations");
	fmt_end(&dump_json_ctx);
}

/*
 * Start a new operation object, the operation and its path are always printed.
 * The mode selects how the path is resolved, same as for the text output.
 */
static int dump_json_op_start(enum print_mode mode, void *user, const char *op,
			      const char *path)
{
	struct btrfs_dump_send_args *r = user;
	char full_path[PATH_MAX];
	const char *out_path;
	int ret;

	if (mode == PRINT_DUMP_SUBVOLUME) {
		PATH_CAT_OR_RET(op, r->full_subvol_path, r->root_path, path, ret);
		out_path = r->full_subvol_path;
	} else {
		PATH_CAT_OR_RET(op, full_path, r->full_subvol_path, path, ret);
		out_path = full_path;
	}

	fmt_print_start_group(&dump_json_ctx, NULL, JSON_TYPE_MAP);
	fmt_print(&dump_json_ctx, "op", op);
	fmt_print(&dump_json_ctx, "path", out_path);
	return 0;
}

/* Close the operation object started by dump_json_op_start() */
static int dump_json_op_end(void)
{
	fmt_print_end_group(&dump_json_ctx, NULL);
	return 0;
}

/* Operation that has no other parameters beside the path */
static int dump_json_op_simple(enum print_mode mode, void *user, const char *op,
			       const char *path)
{
	int ret = dump_json_op_start(mode, user, op, path);

	if (ret < 0)
		return ret;
	return dump_json_op_end();
}

static int print_subvol(const char *path, const u8 *uuid, u64 ctransid,
			void *user)
{
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];

	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_SUBVOLUME, user, "subvol", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "uuid", uuid);
		fmt_print(&dump_json_ctx, "ctransid", ctransid);
		return dump_json_op_end();
	}

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

	if (bconf.output_format == CMD_FORMAT_JSON) {
		ret = dump_json_op_start(PRINT_DUMP_SUBVOLUME, user, "snapshot", path);
		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "uuid", uuid);
		fmt_print(&dump_json_ctx, "ctransid", ctransid);
		fmt_print(&dump_json_ctx, "parent_uuid", parent_uuid);
		fmt_print(&dump_json_ctx, "parent_ctransid", parent_ctransid);
		return dump_json_op_end();
	}

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
	if (bconf.output_format == CMD_FORMAT_JSON)
		return dump_json_op_simple(PRINT_DUMP_NORMAL, user, "mkfile", path);
	return PRINT_DUMP(user, path, "mkfile", NULL);
}

static int print_mkdir(const char *path, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON)
		return dump_json_op_simple(PRINT_DUMP_NORMAL, user, "mkdir", path);
	return PRINT_DUMP(user, path, "mkdir", NULL);
}

static int print_mknod(const char *path, u64 mode, u64 dev, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "mknod", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "mode", mode);
		fmt_print(&dump_json_ctx, "dev", dev);
		return dump_json_op_end();
	}
	return PRINT_DUMP(user, path, "mknod", "mode=%llo dev=0x%llx", mode,
			  dev);
}

static int print_mkfifo(const char *path, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON)
		return dump_json_op_simple(PRINT_DUMP_NORMAL, user, "mkfifo", path);
	return PRINT_DUMP(user, path, "mkfifo", NULL);
}

static int print_mksock(const char *path, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON)
		return dump_json_op_simple(PRINT_DUMP_NORMAL, user, "mksock", path);
	return PRINT_DUMP(user, path, "mksock", NULL);
}

static int print_symlink(const char *path, const char *lnk, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "symlink", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "dest", lnk);
		return dump_json_op_end();
	}

	PRINT_DUMP_NO_NEWLINE(user, path, "symlink", "dest=");
	string_print_escape_special(lnk);
	putchar('\n');
	return 0;
}

static int print_rename(const char *from, const char *to, void *user)
{
	struct btrfs_dump_send_args *r = user;
	char full_to[PATH_MAX];
	int ret;

	PATH_CAT_OR_RET("rename", full_to, r->full_subvol_path, to, ret);
	if (bconf.output_format == CMD_FORMAT_JSON) {
		ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "rename", from);
		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "dest", full_to);
		return dump_json_op_end();
	}

	PRINT_DUMP_NO_NEWLINE(user, from, "rename", "dest=");
	string_print_escape_special(full_to);
	putchar('\n');
	return 0;
}

static int print_link(const char *path, const char *lnk, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "link", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "dest", lnk);
		return dump_json_op_end();
	}

	PRINT_DUMP_NO_NEWLINE(user, path, "link", "dest=");
	string_print_escape_special(lnk);
	putchar('\n');
	return 0;
}

static int print_unlink(const char *path, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON)
		return dump_json_op_simple(PRINT_DUMP_NORMAL, user, "unlink", path);
	return PRINT_DUMP(user, path, "unlink", NULL);
}

static int print_rmdir(const char *path, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON)
		return dump_json_op_simple(PRINT_DUMP_NORMAL, user, "rmdir", path);
	return PRINT_DUMP(user, path, "rmdir", NULL);
}

static int print_write(const char *path, const void *data, u64 offset,
		       u64 len, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "write", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "offset", offset);
		fmt_print(&dump_json_ctx, "length", len);
		return dump_json_op_end();
	}
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

	if (bconf.output_format == CMD_FORMAT_JSON) {
		ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "clone", path);
		if (ret < 0)
			return ret;
		PATH_CAT_OR_RET("clone", full_path, r->full_subvol_path, clone_path, ret);
		fmt_print(&dump_json_ctx, "offset", offset);
		fmt_print(&dump_json_ctx, "length", len);
		fmt_print(&dump_json_ctx, "from", full_path);
		fmt_print(&dump_json_ctx, "clone_uuid", clone_uuid);
		fmt_print(&dump_json_ctx, "clone_ctransid", clone_ctransid);
		fmt_print(&dump_json_ctx, "clone_offset", clone_offset);
		return dump_json_op_end();
	}

	PATH_CAT_OR_RET("clone", full_path, r->full_subvol_path, clone_path, ret);
	PRINT_DUMP_NO_NEWLINE(user, path, "clone", "offset=%llu len=%llu from=", offset, len);
	string_print_escape_special(full_path);
	putchar(' ');
	printf("clone_offset=%llu\n", clone_offset);
	return 0;
}

/*
 * Xattr names are like paths and can potentially contain special characters,
 * xattr values can be arbitrary.
 */
static int print_set_xattr(const char *path, const char *name,
			   const void *data, int len, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "set_xattr", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "name", name);
		fmt_print(&dump_json_ctx, "value", (const char *)data, (unsigned int)len);
		fmt_print(&dump_json_ctx, "length", (u64)len);
		return dump_json_op_end();
	}

	PRINT_DUMP_NO_NEWLINE(user, path, "set_xattr", "name=");
	string_print_escape_special(name);
	putchar(' ');
	string_print_escape_special_len((const char *)data, len);
	putchar(' ');
	printf("len=%d\n", len);
	return 0;
}

static int print_remove_xattr(const char *path, const char *name, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "remove_xattr", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "name", name);
		return dump_json_op_end();
	}

	PRINT_DUMP_NO_NEWLINE(user, path, "remove_xattr", "name=");
	string_print_escape_special(name);
	putchar('\n');
	return 0;
}

static int print_truncate(const char *path, u64 size, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "truncate", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "size", size);
		return dump_json_op_end();
	}
	return PRINT_DUMP(user, path, "truncate", "size=%llu", size);
}

static int print_chmod(const char *path, u64 mode, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "chmod", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "mode", mode);
		return dump_json_op_end();
	}
	return PRINT_DUMP(user, path, "chmod", "mode=%llo", mode);
}

static int print_chown(const char *path, u64 uid, u64 gid, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "chown", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "uid", uid);
		fmt_print(&dump_json_ctx, "gid", gid);
		return dump_json_op_end();
	}
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

	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "utimes", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "atime", at_str);
		fmt_print(&dump_json_ctx, "mtime", mt_str);
		fmt_print(&dump_json_ctx, "ctime", ct_str);
		return dump_json_op_end();
	}
	return PRINT_DUMP(user, path, "utimes", "atime=%s mtime=%s ctime=%s",
			  at_str, mt_str, ct_str);
}

static int print_update_extent(const char *path, u64 offset, u64 len,
			       void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "update_extent", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "offset", offset);
		fmt_print(&dump_json_ctx, "length", len);
		return dump_json_op_end();
	}
	return PRINT_DUMP(user, path, "update_extent", "offset=%llu len=%llu",
			  offset, len);
}

static int print_encoded_write(const char *path, const void *data, u64 offset,
			       u64 len, u64 unencoded_file_len,
			       u64 unencoded_len, u64 unencoded_offset,
			       u32 compression, u32 encryption, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "encoded_write", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "offset", offset);
		fmt_print(&dump_json_ctx, "length", len);
		fmt_print(&dump_json_ctx, "unencoded_file_len", unencoded_file_len);
		fmt_print(&dump_json_ctx, "unencoded_len", unencoded_len);
		fmt_print(&dump_json_ctx, "unencoded_offset", unencoded_offset);
		fmt_print(&dump_json_ctx, "compression", (u64)compression);
		fmt_print(&dump_json_ctx, "encryption", (u64)encryption);
		return dump_json_op_end();
	}
	return PRINT_DUMP(user, path, "encoded_write",
"offset=%llu len=%llu unencoded_file_len=%llu unencoded_len=%llu unencoded_offset=%llu compression=%u encryption=%u",
			  offset, len, unencoded_file_len, unencoded_len,
			  unencoded_offset, compression, encryption);
}

static int print_fallocate(const char *path, int mode, u64 offset, u64 len,
			   void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "fallocate", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "mode", (u64)mode);
		fmt_print(&dump_json_ctx, "offset", offset);
		fmt_print(&dump_json_ctx, "length", len);
		return dump_json_op_end();
	}
	return PRINT_DUMP(user, path, "fallocate",
			  "mode=%d offset=%llu len=%llu",
			  mode, offset, len);
}

static int print_fileattr(const char *path, u64 attr, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "fileattr", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "fileattr", attr);
		return dump_json_op_end();
	}
	return PRINT_DUMP(user, path, "fileattr", "fileattr=0x%llu", attr);
}

static int print_enable_verity (const char *path, u8 algorithm, u32 block_size,
				int salt_len, char *salt,
				int sig_len, char *sig, void *user)
{
	if (bconf.output_format == CMD_FORMAT_JSON) {
		int ret = dump_json_op_start(PRINT_DUMP_NORMAL, user, "enable_verity", path);

		if (ret < 0)
			return ret;
		fmt_print(&dump_json_ctx, "algorithm", (u64)algorithm);
		fmt_print(&dump_json_ctx, "block_size", (u64)block_size);
		fmt_print(&dump_json_ctx, "salt_len", (u64)salt_len);
		fmt_print(&dump_json_ctx, "sig_len", (u64)sig_len);
		return dump_json_op_end();
	}
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
