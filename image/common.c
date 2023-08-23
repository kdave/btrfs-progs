#include "kerncompat.h"
#include <sys/stat.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "crypto/crc32c.h"
#include "common/device-utils.h"
#include "common/messages.h"
#include "image/metadump.h"
#include "image/common.h"

const struct dump_version dump_versions[] = {
	/*
	 * The original format, which only supports tree blocks and free space
	 * cache dump.
	 */
	{ .version = 0,
	  .max_pending_size = SZ_256K,
	  .magic_cpu = 0xbd5c25e27295668bULL,
	  .extra_sb_flags = 1 },
#if EXPERIMENTAL
	/*
	 * The new format, with much larger item size to contain any data
	 * extents.
	 */
	{ .version = 1,
	  .max_pending_size = SZ_256M,
	  .magic_cpu = 0x31765f506d55445fULL, /* ascii _DUmP_v1, no null */
	  .extra_sb_flags = 0 },
#endif
};

const struct dump_version *current_version = &dump_versions[0];

int detect_version(FILE *in)
{
	struct meta_cluster *cluster;
	u8 buf[IMAGE_BLOCK_SIZE];
	bool found = false;
	int i;
	int ret;

	if (fseek(in, 0, SEEK_SET) < 0) {
		error("seek failed: %m");
		return -errno;
	}
	ret = fread(buf, IMAGE_BLOCK_SIZE, 1, in);
	if (!ret) {
		error("failed to read header");
		return -EIO;
	}

	fseek(in, 0, SEEK_SET);
	cluster = (struct meta_cluster *)buf;
	for (i = 0; i < ARRAY_SIZE(dump_versions); i++) {
		if (le64_to_cpu(cluster->header.magic) ==
		    dump_versions[i].magic_cpu) {
			found = true;
			current_version = &dump_versions[i];
			break;
		}
	}

	if (!found) {
		error("unrecognized header format");
		return -EINVAL;
	}
	return 0;
}

void csum_block(u8 *buf, size_t len)
{
	u16 csum_size = btrfs_csum_type_size(BTRFS_CSUM_TYPE_CRC32);
	u8 result[csum_size];
	u32 crc = ~(u32)0;
	crc = crc32c(crc, buf + BTRFS_CSUM_SIZE, len - BTRFS_CSUM_SIZE);
	put_unaligned_le32(~crc, result);
	memcpy(buf, result, csum_size);
}

void write_backup_supers(int fd, u8 *buf)
{
	struct btrfs_super_block *super = (struct btrfs_super_block *)buf;
	struct stat st;
	u64 size;
	u64 bytenr;
	int i;
	int ret;

	if (fstat(fd, &st)) {
		error(
	"cannot stat restore point, won't be able to write backup supers: %m");
		return;
	}

	size = device_get_partition_size_fd_stat(fd, &st);

	for (i = 1; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		if (bytenr + BTRFS_SUPER_INFO_SIZE > size)
			break;
		btrfs_set_super_bytenr(super, bytenr);
		csum_block(buf, BTRFS_SUPER_INFO_SIZE);
		ret = pwrite(fd, buf, BTRFS_SUPER_INFO_SIZE, bytenr);
		if (ret < BTRFS_SUPER_INFO_SIZE) {
			if (ret < 0)
				error(
				"problem writing out backup super block %d: %m", i);
			else
				error("short write writing out backup super block");
			break;
		}
	}
}

int update_disk_super_on_device(struct btrfs_fs_info *info,
				const char *other_dev, u64 cur_devid)
{
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_path path = { 0 };
	struct btrfs_dev_item *dev_item;
	struct btrfs_super_block disk_super;
	char dev_uuid[BTRFS_UUID_SIZE];
	char fs_uuid[BTRFS_UUID_SIZE];
	u64 devid, type, io_align, io_width;
	u64 sector_size, total_bytes, bytes_used;
	int fp = -1;
	int ret;

	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = cur_devid;

	ret = btrfs_search_slot(NULL, info->chunk_root, &key, &path, 0, 0);
	if (ret) {
		error("search key failed: %d", ret);
		ret = -EIO;
		goto out;
	}

	leaf = path.nodes[0];
	dev_item = btrfs_item_ptr(leaf, path.slots[0],
				  struct btrfs_dev_item);

	devid = btrfs_device_id(leaf, dev_item);
	if (devid != cur_devid) {
		error("devid mismatch: %llu != %llu", devid, cur_devid);
		ret = -EIO;
		goto out;
	}

	type = btrfs_device_type(leaf, dev_item);
	io_align = btrfs_device_io_align(leaf, dev_item);
	io_width = btrfs_device_io_width(leaf, dev_item);
	sector_size = btrfs_device_sector_size(leaf, dev_item);
	total_bytes = btrfs_device_total_bytes(leaf, dev_item);
	bytes_used = btrfs_device_bytes_used(leaf, dev_item);
	read_extent_buffer(leaf, dev_uuid, (unsigned long)btrfs_device_uuid(dev_item), BTRFS_UUID_SIZE);
	read_extent_buffer(leaf, fs_uuid, (unsigned long)btrfs_device_fsid(dev_item), BTRFS_UUID_SIZE);

	btrfs_release_path(&path);

	printf("update disk super on %s devid=%llu\n", other_dev, devid);

	/* update other devices' super block */
	fp = open(other_dev, O_CREAT | O_RDWR, 0600);
	if (fp < 0) {
		error("could not open %s: %m", other_dev);
		ret = -EIO;
		goto out;
	}

	memcpy(&disk_super, info->super_copy, BTRFS_SUPER_INFO_SIZE);

	dev_item = &disk_super.dev_item;

	btrfs_set_stack_device_type(dev_item, type);
	btrfs_set_stack_device_id(dev_item, devid);
	btrfs_set_stack_device_total_bytes(dev_item, total_bytes);
	btrfs_set_stack_device_bytes_used(dev_item, bytes_used);
	btrfs_set_stack_device_io_align(dev_item, io_align);
	btrfs_set_stack_device_io_width(dev_item, io_width);
	btrfs_set_stack_device_sector_size(dev_item, sector_size);
	memcpy(dev_item->uuid, dev_uuid, BTRFS_UUID_SIZE);
	memcpy(dev_item->fsid, fs_uuid, BTRFS_UUID_SIZE);
	csum_block((u8 *)&disk_super, BTRFS_SUPER_INFO_SIZE);

	ret = pwrite(fp, &disk_super, BTRFS_SUPER_INFO_SIZE, BTRFS_SUPER_INFO_OFFSET);
	if (ret != BTRFS_SUPER_INFO_SIZE) {
		if (ret < 0) {
			errno = ret;
			error("cannot write superblock: %m");
		} else {
			error("cannot write superblock");
		}
		ret = -EIO;
		goto out;
	}

	write_backup_supers(fp, (u8 *)&disk_super);

out:
	if (fp != -1)
		close(fp);
	return ret;
}
