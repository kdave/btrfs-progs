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

#include <uuid/uuid.h>
#include <unistd.h>

#include "send.h"
#include "send-stream.h"
#include "crc32c.h"

struct btrfs_send_stream {
	int fd;
	char read_buf[BTRFS_SEND_BUF_SIZE];

	int cmd;
	struct btrfs_cmd_header *cmd_hdr;
	struct btrfs_tlv_header *cmd_attrs[BTRFS_SEND_A_MAX + 1];
	u32 version;

	struct btrfs_send_ops *ops;
	void *user;
};

static int read_buf(struct btrfs_send_stream *s, void *buf, int len)
{
	int ret;
	int pos = 0;

	while (pos < len) {
		ret = read(s->fd, (char*)buf + pos, len - pos);
		if (ret < 0) {
			ret = -errno;
			fprintf(stderr, "ERROR: read from stream failed. %s\n",
					strerror(-ret));
			goto out;
		}
		if (ret == 0) {
			ret = 1;
			goto out;
		}
		pos += ret;
	}

	ret = 0;

out:
	return ret;
}

/*
 * Reads a single command from kernel space and decodes the TLV's into
 * s->cmd_attrs
 */
static int read_cmd(struct btrfs_send_stream *s)
{
	int ret;
	int cmd;
	int cmd_len;
	int tlv_type;
	int tlv_len;
	char *data;
	int pos;
	struct btrfs_tlv_header *tlv_hdr;
	u32 crc;
	u32 crc2;

	memset(s->cmd_attrs, 0, sizeof(s->cmd_attrs));

	ret = read_buf(s, s->read_buf, sizeof(*s->cmd_hdr));
	if (ret < 0)
		goto out;
	if (ret) {
		ret = -EINVAL;
		fprintf(stderr, "ERROR: unexpected EOF in stream.\n");
		goto out;
	}

	s->cmd_hdr = (struct btrfs_cmd_header *)s->read_buf;
	cmd = le16_to_cpu(s->cmd_hdr->cmd);
	cmd_len = le32_to_cpu(s->cmd_hdr->len);

	data = s->read_buf + sizeof(*s->cmd_hdr);
	ret = read_buf(s, data, cmd_len);
	if (ret < 0)
		goto out;
	if (ret) {
		ret = -EINVAL;
		fprintf(stderr, "ERROR: unexpected EOF in stream.\n");
		goto out;
	}

	crc = le32_to_cpu(s->cmd_hdr->crc);
	s->cmd_hdr->crc = 0;

	crc2 = crc32c(0, (unsigned char*)s->read_buf,
			sizeof(*s->cmd_hdr) + cmd_len);

	if (crc != crc2) {
		ret = -EINVAL;
		fprintf(stderr, "ERROR: crc32 mismatch in command.\n");
		goto out;
	}

	pos = 0;
	while (pos < cmd_len) {
		tlv_hdr = (struct btrfs_tlv_header *)data;
		tlv_type = le16_to_cpu(tlv_hdr->tlv_type);
		tlv_len = le16_to_cpu(tlv_hdr->tlv_len);

		if (tlv_type <= 0 || tlv_type > BTRFS_SEND_A_MAX ||
		    tlv_len < 0 || tlv_len > BTRFS_SEND_BUF_SIZE) {
			fprintf(stderr, "ERROR: invalid tlv in cmd. "
					"tlv_type = %d, tlv_len = %d\n",
					tlv_type, tlv_len);
			ret = -EINVAL;
			goto out;
		}

		s->cmd_attrs[tlv_type] = tlv_hdr;

		data += sizeof(*tlv_hdr) + tlv_len;
		pos += sizeof(*tlv_hdr) + tlv_len;
	}

	s->cmd = cmd;
	ret = 0;

out:
	return ret;
}

static int tlv_get(struct btrfs_send_stream *s, int attr, void **data, int *len)
{
	int ret;
	struct btrfs_tlv_header *h;

	if (attr <= 0 || attr > BTRFS_SEND_A_MAX) {
		fprintf(stderr, "ERROR: invalid attribute requested. "
				"attr = %d\n",
				attr);
		ret = -EINVAL;
		goto out;
	}

	h = s->cmd_attrs[attr];
	if (!h) {
		fprintf(stderr, "ERROR: attribute %d requested "
				"but not present.\n", attr);
		ret = -ENOENT;
		goto out;
	}

	*len = le16_to_cpu(h->tlv_len);
	*data = h + 1;

	ret = 0;

out:
	return ret;
}

#define __TLV_GOTO_FAIL(expr) \
	if ((ret = expr) < 0) \
		goto tlv_get_failed;

#define __TLV_DO_WHILE_GOTO_FAIL(expr) \
	do { \
		__TLV_GOTO_FAIL(expr) \
	} while (0)


#define TLV_GET(s, attr, data, len) \
	__TLV_DO_WHILE_GOTO_FAIL(tlv_get(s, attr, data, len))

#define TLV_CHECK_LEN(expected, got) \
	do { \
		if (expected != got) { \
			fprintf(stderr, "ERROR: invalid size for attribute. " \
					"expected = %d, got = %d\n", \
					(int)expected, (int)got); \
			ret = -EINVAL; \
			goto tlv_get_failed; \
		} \
	} while (0)

#define TLV_GET_INT(s, attr, bits, v) \
	do { \
		__le##bits *__tmp; \
		int __len; \
		TLV_GET(s, attr, (void**)&__tmp, &__len); \
		TLV_CHECK_LEN(sizeof(*__tmp), __len); \
		*v = get_unaligned_le##bits(__tmp); \
	} while (0)

#define TLV_GET_U8(s, attr, v) TLV_GET_INT(s, attr, 8, v)
#define TLV_GET_U16(s, attr, v) TLV_GET_INT(s, attr, 16, v)
#define TLV_GET_U32(s, attr, v) TLV_GET_INT(s, attr, 32, v)
#define TLV_GET_U64(s, attr, v) TLV_GET_INT(s, attr, 64, v)

static int tlv_get_string(struct btrfs_send_stream *s, int attr, char **str)
{
	int ret;
	void *data;
	int len = 0;

	TLV_GET(s, attr, &data, &len);

	*str = malloc(len + 1);
	if (!*str)
		return -ENOMEM;

	memcpy(*str, data, len);
	(*str)[len] = 0;
	ret = 0;

tlv_get_failed:
	return ret;
}
#define TLV_GET_STRING(s, attr, str) \
	__TLV_DO_WHILE_GOTO_FAIL(tlv_get_string(s, attr, str))

static int tlv_get_timespec(struct btrfs_send_stream *s,
			    int attr, struct timespec *ts)
{
	int ret;
	int len;
	struct btrfs_timespec *bts;

	TLV_GET(s, attr, (void**)&bts, &len);
	TLV_CHECK_LEN(sizeof(*bts), len);

	ts->tv_sec = le64_to_cpu(bts->sec);
	ts->tv_nsec = le32_to_cpu(bts->nsec);
	ret = 0;

tlv_get_failed:
	return ret;
}
#define TLV_GET_TIMESPEC(s, attr, ts) \
	__TLV_DO_WHILE_GOTO_FAIL(tlv_get_timespec(s, attr, ts))

static int tlv_get_uuid(struct btrfs_send_stream *s, int attr, u8 *uuid)
{
	int ret;
	int len;
	void *data;

	TLV_GET(s, attr, &data, &len);
	TLV_CHECK_LEN(BTRFS_UUID_SIZE, len);
	memcpy(uuid, data, BTRFS_UUID_SIZE);

	ret = 0;

tlv_get_failed:
	return ret;
}
#define TLV_GET_UUID(s, attr, uuid) \
	__TLV_DO_WHILE_GOTO_FAIL(tlv_get_uuid(s, attr, uuid))

static int read_and_process_cmd(struct btrfs_send_stream *s)
{
	int ret;
	char *path = NULL;
	char *path_to = NULL;
	char *clone_path = NULL;
	char *xattr_name = NULL;
	void *xattr_data = NULL;
	void *data = NULL;
	struct timespec at;
	struct timespec ct;
	struct timespec mt;
	u8 uuid[BTRFS_UUID_SIZE];
	u8 clone_uuid[BTRFS_UUID_SIZE];
	u64 tmp;
	u64 tmp2;
	u64 ctransid;
	u64 clone_ctransid;
	u64 mode;
	u64 dev;
	u64 clone_offset;
	u64 offset;
	int len;
	int xattr_len;

	ret = read_cmd(s);
	if (ret)
		goto out;

	switch (s->cmd) {
	case BTRFS_SEND_C_SUBVOL:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_UUID(s, BTRFS_SEND_A_UUID, uuid);
		TLV_GET_U64(s, BTRFS_SEND_A_CTRANSID, &ctransid);
		ret = s->ops->subvol(path, uuid, ctransid, s->user);
		break;
	case BTRFS_SEND_C_SNAPSHOT:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_UUID(s, BTRFS_SEND_A_UUID, uuid);
		TLV_GET_U64(s, BTRFS_SEND_A_CTRANSID, &ctransid);
		TLV_GET_UUID(s, BTRFS_SEND_A_CLONE_UUID, clone_uuid);
		TLV_GET_U64(s, BTRFS_SEND_A_CLONE_CTRANSID, &clone_ctransid);
		ret = s->ops->snapshot(path, uuid, ctransid, clone_uuid,
				clone_ctransid, s->user);
		break;
	case BTRFS_SEND_C_MKFILE:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		ret = s->ops->mkfile(path, s->user);
		break;
	case BTRFS_SEND_C_MKDIR:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		ret = s->ops->mkdir(path, s->user);
		break;
	case BTRFS_SEND_C_MKNOD:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(s, BTRFS_SEND_A_MODE, &mode);
		TLV_GET_U64(s, BTRFS_SEND_A_RDEV, &dev);
		ret = s->ops->mknod(path, mode, dev, s->user);
		break;
	case BTRFS_SEND_C_MKFIFO:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		ret = s->ops->mkfifo(path, s->user);
		break;
	case BTRFS_SEND_C_MKSOCK:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		ret = s->ops->mksock(path, s->user);
		break;
	case BTRFS_SEND_C_SYMLINK:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH_LINK, &path_to);
		ret = s->ops->symlink(path, path_to, s->user);
		break;
	case BTRFS_SEND_C_RENAME:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH_TO, &path_to);
		ret = s->ops->rename(path, path_to, s->user);
		break;
	case BTRFS_SEND_C_LINK:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH_LINK, &path_to);
		ret = s->ops->link(path, path_to, s->user);
		break;
	case BTRFS_SEND_C_UNLINK:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		ret = s->ops->unlink(path, s->user);
		break;
	case BTRFS_SEND_C_RMDIR:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		ret = s->ops->rmdir(path, s->user);
		break;
	case BTRFS_SEND_C_WRITE:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(s, BTRFS_SEND_A_FILE_OFFSET, &offset);
		TLV_GET(s, BTRFS_SEND_A_DATA, &data, &len);
		ret = s->ops->write(path, data, offset, len, s->user);
		break;
	case BTRFS_SEND_C_CLONE:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(s, BTRFS_SEND_A_FILE_OFFSET, &offset);
		TLV_GET_U64(s, BTRFS_SEND_A_CLONE_LEN, &len);
		TLV_GET_UUID(s, BTRFS_SEND_A_CLONE_UUID, clone_uuid);
		TLV_GET_U64(s, BTRFS_SEND_A_CLONE_CTRANSID, &clone_ctransid);
		TLV_GET_STRING(s, BTRFS_SEND_A_CLONE_PATH, &clone_path);
		TLV_GET_U64(s, BTRFS_SEND_A_CLONE_OFFSET, &clone_offset);
		ret = s->ops->clone(path, offset, len, clone_uuid,
				clone_ctransid, clone_path, clone_offset,
				s->user);
		break;
	case BTRFS_SEND_C_SET_XATTR:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_STRING(s, BTRFS_SEND_A_XATTR_NAME, &xattr_name);
		TLV_GET(s, BTRFS_SEND_A_XATTR_DATA, &xattr_data, &xattr_len);
		ret = s->ops->set_xattr(path, xattr_name, xattr_data,
				xattr_len, s->user);
		break;
	case BTRFS_SEND_C_REMOVE_XATTR:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_STRING(s, BTRFS_SEND_A_XATTR_NAME, &xattr_name);
		ret = s->ops->remove_xattr(path, xattr_name, s->user);
		break;
	case BTRFS_SEND_C_TRUNCATE:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(s, BTRFS_SEND_A_SIZE, &tmp);
		ret = s->ops->truncate(path, tmp, s->user);
		break;
	case BTRFS_SEND_C_CHMOD:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(s, BTRFS_SEND_A_MODE, &tmp);
		ret = s->ops->chmod(path, tmp, s->user);
		break;
	case BTRFS_SEND_C_CHOWN:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(s, BTRFS_SEND_A_UID, &tmp);
		TLV_GET_U64(s, BTRFS_SEND_A_GID, &tmp2);
		ret = s->ops->chown(path, tmp, tmp2, s->user);
		break;
	case BTRFS_SEND_C_UTIMES:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_TIMESPEC(s, BTRFS_SEND_A_ATIME, &at);
		TLV_GET_TIMESPEC(s, BTRFS_SEND_A_MTIME, &mt);
		TLV_GET_TIMESPEC(s, BTRFS_SEND_A_CTIME, &ct);
		ret = s->ops->utimes(path, &at, &mt, &ct, s->user);
		break;
	case BTRFS_SEND_C_UPDATE_EXTENT:
		TLV_GET_STRING(s, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(s, BTRFS_SEND_A_FILE_OFFSET, &offset);
		TLV_GET_U64(s, BTRFS_SEND_A_SIZE, &tmp);
		ret = s->ops->update_extent(path, offset, tmp, s->user);
		break;
	case BTRFS_SEND_C_END:
		ret = 1;
		break;
	}

tlv_get_failed:
out:
	free(path);
	free(path_to);
	free(clone_path);
	free(xattr_name);
	return ret;
}

/*
 * If max_errors is 0, then don't stop processing the stream if one of the
 * callbacks in btrfs_send_ops structure returns an error. If greater than
 * zero, stop after max_errors errors happened.
 */
int btrfs_read_and_process_send_stream(int fd,
				       struct btrfs_send_ops *ops, void *user,
				       int honor_end_cmd,
				       u64 max_errors)
{
	int ret;
	struct btrfs_send_stream s;
	struct btrfs_stream_header hdr;
	u64 errors = 0;
	int last_err = 0;

	s.fd = fd;
	s.ops = ops;
	s.user = user;

	ret = read_buf(&s, &hdr, sizeof(hdr));
	if (ret < 0)
		goto out;
	if (ret) {
		ret = 1;
		goto out;
	}

	if (strcmp(hdr.magic, BTRFS_SEND_STREAM_MAGIC)) {
		ret = -EINVAL;
		fprintf(stderr, "ERROR: Unexpected header\n");
		goto out;
	}

	s.version = le32_to_cpu(hdr.version);
	if (s.version > BTRFS_SEND_STREAM_VERSION) {
		ret = -EINVAL;
		fprintf(stderr, "ERROR: Stream version %d not supported. "
				"Please upgrade btrfs-progs\n", s.version);
		goto out;
	}

	while (1) {
		ret = read_and_process_cmd(&s);
		if (ret < 0) {
			last_err = ret;
			errors++;
			if (max_errors > 0 && errors >= max_errors)
				goto out;
		} else if (ret > 0) {
			if (!honor_end_cmd)
				ret = 0;
			goto out;
		}
	}

out:
	if (last_err && !ret)
		ret = last_err;

	return ret;
}
