/*
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
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include <limits.h>
#include <byteswap.h>
#include "crypto/crc32c.h"
#include "kernel-shared/disk-io.h"

#define BLOCKSIZE (4096)
static char buf[BLOCKSIZE];
static int csum_size;

static int check_csum_superblock(void *sb)
{
	u8 result[BTRFS_CSUM_SIZE];
	u16 csum_type = btrfs_super_csum_type(sb);

	btrfs_csum_data(NULL, csum_type, (unsigned char *)sb + BTRFS_CSUM_SIZE,
			result, BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);

	return !memcmp(sb, result, csum_size);
}

static void update_block_csum(void *block)
{
	u8 result[BTRFS_CSUM_SIZE];
	struct btrfs_header *hdr;
	u16 csum_type = btrfs_super_csum_type(block);

	btrfs_csum_data(NULL, csum_type, (unsigned char *)block + BTRFS_CSUM_SIZE,
			result, BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);

	memset(block, 0, BTRFS_CSUM_SIZE);
	hdr = (struct btrfs_header *)block;
	memcpy(&hdr->csum, result, csum_size);
}

static u64 arg_strtou64(const char *str)
{
        u64 value;
        char *ptr_parse_end = NULL;

        value = strtoull(str, &ptr_parse_end, 0);
        if (ptr_parse_end && *ptr_parse_end != '\0') {
                fprintf(stderr, "ERROR: %s is not a valid numeric value.\n",
                        str);
                exit(1);
        }

        /*
         * if we pass a negative number to strtoull, it will return an
         * unexpected number to us, so let's do the check ourselves.
         */
        if (str[0] == '-') {
                fprintf(stderr, "ERROR: %s: negative value is invalid.\n",
                        str);
                exit(1);
        }
        if (value == ULLONG_MAX) {
                fprintf(stderr, "ERROR: %s is too large.\n", str);
                exit(1);
        }
        return value;
}

enum field_op {
	OP_GET,
	OP_SET,
	OP_ADD,
	OP_SUB,
	OP_XOR,
	OP_NAND,	/* broken */
	OP_BSWAP,
};

struct fspec {
	const char *name;
	enum field_op fop;
	u64 value;
};

enum field_type {
	TYPE_UNKNOWN,
	TYPE_U8,
	TYPE_U16,
	TYPE_U32,
	TYPE_U64,
};

struct sb_field {
	const char *name;
	enum field_type type;
} known_fields[] = {
	{ .name = "bytenr",			.type = TYPE_U64 },
	{ .name = "flags",			.type = TYPE_U64 },
	{ .name = "magic",			.type = TYPE_U64 },
	{ .name = "generation",			.type = TYPE_U64 },
	{ .name = "root",			.type = TYPE_U64 },
	{ .name = "chunk_root",			.type = TYPE_U64 },
	{ .name = "log_root",			.type = TYPE_U64 },
	{ .name = "log_root_transid",		.type = TYPE_U64 },
	{ .name = "total_bytes",		.type = TYPE_U64 },
	{ .name = "bytes_used",			.type = TYPE_U64 },
	{ .name = "root_dir_objectid",		.type = TYPE_U64 },
	{ .name = "num_devices",		.type = TYPE_U64 },
	{ .name = "sectorsize",			.type = TYPE_U32 },
	{ .name = "nodesize",			.type = TYPE_U32 },
	{ .name = "stripesize",			.type = TYPE_U32 },
	{ .name = "sys_chunk_array_size",	.type = TYPE_U32 },
	{ .name = "chunk_root_generation",	.type = TYPE_U64 },
	{ .name = "compat_flags",		.type = TYPE_U64 },
	{ .name = "compat_ro_flags",		.type = TYPE_U64 },
	{ .name = "incompat_flags",		.type = TYPE_U64 },
	{ .name = "csum_type",			.type = TYPE_U16 },
	{ .name = "root_level",			.type = TYPE_U8 },
	{ .name = "chunk_root_level",		.type = TYPE_U8 },
	{ .name = "log_root_level",		.type = TYPE_U8 },
	{ .name = "cache_generation",		.type = TYPE_U64 },
	{ .name = "uuid_tree_generation",	.type = TYPE_U64 },
	/* Device item members  */
	{ .name = "dev_item.devid",		.type = TYPE_U64 },
	{ .name = "dev_item.total_bytes",	.type = TYPE_U64 },
	{ .name = "dev_item.bytes_used",	.type = TYPE_U64 },
	{ .name = "dev_item.io_align",		.type = TYPE_U32 },
	{ .name = "dev_item.io_width",		.type = TYPE_U32 },
	{ .name = "dev_item.sector_size",	.type = TYPE_U32 },
	{ .name = "dev_item.type",		.type = TYPE_U64 },
	{ .name = "dev_item.generation",	.type = TYPE_U64 },
	{ .name = "dev_item.start_offset",	.type = TYPE_U64 },
	{ .name = "dev_item.dev_group",		.type = TYPE_U32 },
	{ .name = "dev_item.seek_speed",	.type = TYPE_U8 },
	{ .name = "dev_item.bandwidth",		.type = TYPE_U8 },
};

#define MOD_FIELD_XX(fname, set, val, bits, f_dec, f_hex, f_type)	\
	else if (strcmp(name, #fname) == 0) {				\
		if (set) {						\
			printf("SET: "#fname" "f_dec" (0x"f_hex")\n", \
			(f_type)*val, (f_type)*val);				\
			sb->fname = cpu_to_le##bits(*val);			\
		} else {							\
			*val = le##bits##_to_cpu(sb->fname);			\
			printf("GET: "#fname" "f_dec" (0x"f_hex")\n", 	\
			(f_type)*val, (f_type)*val);			\
		}							\
	}

#define MOD_DEV_FIELD_XX(fname, set, val, bits, f_dec, f_hex, f_type)	\
	else if (strcmp(name, "dev_item." #fname) == 0) {		\
		if (set) {						\
			printf("SET: dev_item."#fname" "f_dec" (0x"f_hex")\n",	\
			(f_type)*val, (f_type)*val);			\
			sb->dev_item.fname = cpu_to_le##bits(*val);	\
		} else {						\
			*val = le##bits##_to_cpu(sb->dev_item.fname);	\
			printf("GET: dev_item."#fname" "f_dec" (0x"f_hex")\n", 	\
			(f_type)*val, (f_type)*val);			\
		}							\
	}

#define MOD_FIELD64(fname, set, val)					\
	MOD_FIELD_XX(fname, set, val, 64, "%llu", "%llx", unsigned long long)

#define MOD_DEV_FIELD64(fname, set, val)					\
	MOD_DEV_FIELD_XX(fname, set, val, 64, "%llu", "%llx", unsigned long long)

/* Alias for u64 */
#define MOD_FIELD(fname, set, val)	MOD_FIELD64(fname, set, val)
#define MOD_DEV_FIELD(fname, set, val)	MOD_DEV_FIELD64(fname, set, val)

/*
 * Support only GET and SET properly, ADD and SUB may work
 */
#define MOD_FIELD32(fname, set, val)					\
	MOD_FIELD_XX(fname, set, val, 32, "%u", "%x", unsigned int)

#define MOD_FIELD16(fname, set, val)					\
	MOD_FIELD_XX(fname, set, val, 16, "%hu", "%hx", unsigned short int)

#define MOD_FIELD8(fname, set, val)					\
	MOD_FIELD_XX(fname, set, val, 8, "%hhu", "%hhx", unsigned char)

#define MOD_DEV_FIELD32(fname, set, val)				\
	MOD_DEV_FIELD_XX(fname, set, val, 32, "%u", "%x", unsigned int)

#define MOD_DEV_FIELD16(fname, set, val)				\
	MOD_DEV_FIELD_XX(fname, set, val, 16, "%hu", "%hx", unsigned short int)

#define MOD_DEV_FIELD8(fname, set, val)					\
	MOD_DEV_FIELD_XX(fname, set, val, 8, "%hhu", "%hhx", unsigned char)

static bool op_is_write(enum field_op op)
{
	return op != OP_GET;
}

static const char * const type_to_string(enum field_type type)
{
	switch (type) {
	case TYPE_U8:	return "u8";
	case TYPE_U16:	return "u16";
	case TYPE_U32:	return "u32";
	case TYPE_U64:	return "u64";
	case TYPE_UNKNOWN:
	}
	return "UNKNOWN";
}

static void mod_field_by_name(struct btrfs_super_block *sb, int set, const char *name,
		u64 *val)
{
	if (0) { }
		MOD_FIELD(bytenr, set, val)
		MOD_FIELD(flags, set, val)
		MOD_FIELD(magic, set, val)
		MOD_FIELD(generation, set, val)
		MOD_FIELD(root, set, val)
		MOD_FIELD(chunk_root, set, val)
		MOD_FIELD(log_root, set, val)
		MOD_FIELD(log_root_transid, set, val)
		MOD_FIELD(total_bytes, set, val)
		MOD_FIELD(bytes_used, set, val)
		MOD_FIELD(root_dir_objectid, set, val)
		MOD_FIELD(num_devices, set, val)
		MOD_FIELD32(sectorsize, set, val)
		MOD_FIELD32(nodesize, set, val)
		MOD_FIELD32(stripesize, set, val)
		MOD_FIELD32(sys_chunk_array_size, set, val)
		MOD_FIELD(chunk_root_generation, set, val)
		MOD_FIELD(compat_flags, set, val)
		MOD_FIELD(compat_ro_flags, set, val)
		MOD_FIELD(incompat_flags, set, val)
		MOD_FIELD16(csum_type, set, val)
		MOD_FIELD8(root_level, set, val)
		MOD_FIELD8(chunk_root_level, set, val)
		MOD_FIELD8(log_root_level, set, val)
		MOD_FIELD(cache_generation, set, val)
		MOD_FIELD(uuid_tree_generation, set, val)
		MOD_DEV_FIELD(devid, set, val)
		MOD_DEV_FIELD(total_bytes, set, val)
		MOD_DEV_FIELD(bytes_used, set, val)
		MOD_DEV_FIELD32(io_align, set, val)
		MOD_DEV_FIELD32(io_width, set, val)
		MOD_DEV_FIELD32(sector_size, set, val)
		MOD_DEV_FIELD(type, set, val)
		MOD_DEV_FIELD(generation, set, val)
		MOD_DEV_FIELD(start_offset, set, val)
		MOD_DEV_FIELD32(dev_group, set, val)
		MOD_DEV_FIELD8(seek_speed, set, val)
		MOD_DEV_FIELD8(bandwidth, set, val)
	else {
		printf("ERROR: unhandled field: %s\n", name);
		exit(1);
	}
}

static int sb_edit(struct btrfs_super_block *sb, struct fspec *fsp)
{
	u64 val;
	u64 newval;

	mod_field_by_name(sb, 0, fsp->name, &val);
	switch (fsp->fop) {
	case OP_GET: newval = val; break;
	case OP_SET: newval = fsp->value; break;
	case OP_ADD: newval = val + fsp->value; break;
	case OP_SUB: newval = val - fsp->value; break;
	case OP_XOR: newval = val ^ fsp->value; break;
	case OP_NAND: newval = val & (~fsp->value); break;
	case OP_BSWAP: newval = bswap_64(val); break;
	default: printf("ERROR: unhandled operation: %d\n", fsp->fop); exit(1);
	}

	mod_field_by_name(sb, 1, fsp->name, &newval);

	return 0;
}

static int is_known_field(const char *f)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(known_fields); i++)
		if (strcmp(f, known_fields[i].name) == 0)
			return 1;
	return 0;
}

static int arg_to_op_value(const char *arg, enum field_op *op, u64 *val)
{
	switch (arg[0]) {
	case 0: return -1;
	case '.':
	case '?': *op = OP_GET; *val = 0; break;
	case '=': *op = OP_SET; *val = arg_strtou64(arg + 1); break;
	case '+': *op = OP_ADD; *val = arg_strtou64(arg + 1); break;
	case '-': *op = OP_SUB; *val = arg_strtou64(arg + 1); break;
	case '^': *op = OP_XOR; *val = arg_strtou64(arg + 1); break;
	case '~': *op = OP_NAND; *val = arg_strtou64(arg + 1); break;
	case '@': *op = OP_BSWAP; *val = arg_strtou64(arg + 1); break;
	default:
		  printf("ERROR: unknown op: %c\n", arg[0]);
		  return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int fd;
	loff_t off;
	int ret;
	struct btrfs_header *hdr;
	struct btrfs_super_block *sb;
	int i;
	struct fspec spec[128];
	int specidx;
	int changed;

	memset(spec, 0, sizeof(spec));
	if (argc <= 2) {
		printf("Usage: %s [options] image [fieldspec...]\n", argv[0]);
		printf("\n");
		printf("Modify or read a a member of the primary superblock on a given image (file or block device),\n");
		printf("checksum is recalculated after any modification (ie. it is not when just reading the values).\n");
		printf("Use 'btrfs inspect dump-super image' to read the whole superblock\n");
		printf("\n");
		printf("fileldspec is a sequence of pairs 'member op':\n");
		printf("  member: name of the superblock member, listed below\n");
		printf("  op: single character optionally followed by a value (eg. =0x42)\n");
		printf("    . read the member value (no value)\n");
		printf("    ? read the member value (no value)\n");
		printf("    = set member to the exact value (value required)\n");
		printf("    + add this value to member (value required)\n");
		printf("    - subtract this value from member (value required)\n");
		printf("    ^ xor member with this value (value required)\n");
		printf("    @ byteswap of the member (no value)\n");
		printf("\n");
		printf("  member (type)\n");

		for (i = 0; i < ARRAY_SIZE(known_fields); i++) {
			const int width = 24;

			printf("    %-*s  %s\n", width, known_fields[i].name,
					type_to_string(known_fields[i].type));
		}
		exit(1);
	}
	fd = open(argv[1], O_RDWR | O_EXCL);
	if (fd == -1) {
		perror("open()");
		exit(1);
	}

	/* verify superblock */
	csum_size = btrfs_csum_type_size(BTRFS_CSUM_TYPE_CRC32);
	off = BTRFS_SUPER_INFO_OFFSET;

	ret = pread(fd, buf, BLOCKSIZE, off);
	if (ret <= 0) {
		printf("pread error %d at offset %llu\n",
				ret, (unsigned long long)off);
		exit(1);
	}
	if (ret != BLOCKSIZE) {
		printf("pread error at offset %llu: read only %d bytes\n",
				(unsigned long long)off, ret);
		exit(1);
	}
	hdr = (struct btrfs_header *)buf;
	/* verify checksum */
	if (!check_csum_superblock(&hdr->csum)) {
		printf("super block checksum does not match at offset %llu, will be corrected after write\n",
				(unsigned long long)off);
	} else {
		printf("super block checksum is ok\n");
	}
	sb = (struct btrfs_super_block *)buf;

	specidx = 0;
	for (i = 2; i < argc; i++) {
		struct fspec *f;

		if (i + 1 >= argc) {
			printf("ERROR: bad argument count\n");
			ret = 1;
			goto out;
		}

		if (!is_known_field(argv[i])) {
			printf("ERROR: unknown filed: %s\n", argv[i]);
			ret = 1;
			goto out;
		}
		f = &spec[specidx];
		specidx++;
		f->name = strdup(argv[i]);
		i++;
		if (arg_to_op_value(argv[i], &f->fop, &f->value)) {
			ret = 1;
			goto out;
		}
	}

	changed = 0;
	for (i = 0; i < specidx; i++) {
		sb_edit(sb, &spec[i]);
		if (op_is_write(spec[i].fop))
			changed = 1;
	}

	if (changed) {
		printf("Update csum\n");
		update_block_csum(buf);
		ret = pwrite(fd, buf, BLOCKSIZE, off);
		if (ret <= 0) {
			printf("pwrite error %d at offset %llu\n",
					ret, (unsigned long long)off);
			exit(1);
		}
		if (ret != BLOCKSIZE) {
			printf("pwrite error at offset %llu: written only %d bytes\n",
					(unsigned long long)off, ret);
			exit(1);
		}
		fsync(fd);
	} else {
		printf("Nothing changed\n");
	}
	ret = 0;
out:
	close(fd);
	return ret;
}
