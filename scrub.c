/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

/*
 * Main part to implement offline(unmounted) btrfs scrub
 */

#include <unistd.h>
#include "ctree.h"
#include "volumes.h"
#include "disk-io.h"
#include "utils.h"
#include "kernel-lib/bitops.h"

/*
 * For parity based profile (RAID56)
 * Mirror/stripe based on won't need this. They are iterated by bytenr and
 * mirror number.
 */
struct scrub_stripe {
	/* For P/Q logical start will be BTRFS_RAID5/6_P/Q_STRIPE */
	u64 logical;

	u64 physical;

	/* Device is missing */
	unsigned int dev_missing:1;

	/* Any tree/data csum mismatches */
	unsigned int csum_mismatch:1;

	/* Some data doesn't have csum (nodatasum) */
	unsigned int csum_missing:1;

	/* Device fd, to write correct data back to disc */
	int fd;

	char *data;
};

/*
 * RAID56 full stripe (data stripes + P/Q)
 */
struct scrub_full_stripe {
	u64 logical_start;
	u64 logical_len;
	u64 bg_type;
	u32 nr_stripes;
	u32 stripe_len;

	/* Read error stripes */
	u32 err_read_stripes;

	/* Missing devices */
	u32 err_missing_devs;

	/* Csum error data stripes */
	u32 err_csum_dstripes;

	/* Missing csum data stripes */
	u32 missing_csum_dstripes;

	/* currupted stripe index */
	int corrupted_index[2];

	int nr_corrupted_stripes;

	/* Already recovered once? */
	unsigned int recovered:1;

	struct scrub_stripe stripes[];
};

static void free_full_stripe(struct scrub_full_stripe *fstripe)
{
	int i;

	for (i = 0; i < fstripe->nr_stripes; i++)
		free(fstripe->stripes[i].data);
	free(fstripe);
}

static struct scrub_full_stripe *alloc_full_stripe(int nr_stripes,
						    u32 stripe_len)
{
	struct scrub_full_stripe *ret;
	int size = sizeof(*ret) + sizeof(unsigned long *) +
		nr_stripes * sizeof(struct scrub_stripe);
	int i;

	ret = malloc(size);
	if (!ret)
		return NULL;

	memset(ret, 0, size);
	ret->nr_stripes = nr_stripes;
	ret->stripe_len = stripe_len;
	ret->corrupted_index[0] = -1;
	ret->corrupted_index[1] = -1;

	/* Alloc data memory for each stripe */
	for (i = 0; i < nr_stripes; i++) {
		struct scrub_stripe *stripe = &ret->stripes[i];

		stripe->data = malloc(stripe_len);
		if (!stripe->data) {
			free_full_stripe(ret);
			return NULL;
		}
	}
	return ret;
}

static inline int is_data_stripe(struct scrub_stripe *stripe)
{
	u64 bytenr = stripe->logical;

	if (bytenr == BTRFS_RAID5_P_STRIPE || bytenr == BTRFS_RAID6_Q_STRIPE)
		return 0;
	return 1;
}

/*
 * Check one tree mirror given by @bytenr and @mirror, or @data.
 * If @data is not given (NULL), the function will try to read out tree block
 * using @bytenr and @mirror.
 * If @data is given, use data directly, won't try to read from disk.
 *
 * The extra @data prameter is handy for RAID5/6 recovery code to verify
 * the recovered data.
 *
 * Return 0 if everything is OK.
 * Return <0 something goes wrong, and @scrub_ctx accounting will be updated
 * if it's a data corruption.
 */
static int check_tree_mirror(struct btrfs_fs_info *fs_info,
			     struct btrfs_scrub_progress *scrub_ctx,
			     char *data, u64 bytenr, int mirror)
{
	struct extent_buffer *eb;
	u32 nodesize = fs_info->nodesize;
	int ret;

	if (!IS_ALIGNED(bytenr, fs_info->sectorsize)) {
		/* Such error will be reported by check_tree_block() */
		scrub_ctx->verify_errors++;
		return -EIO;
	}

	eb = btrfs_find_create_tree_block(fs_info, bytenr, nodesize);
	if (!eb)
		return -ENOMEM;
	if (data) {
		memcpy(eb->data, data, nodesize);
	} else {
		ret = read_whole_eb(fs_info, eb, mirror);
		if (ret) {
			scrub_ctx->read_errors++;
			error("failed to read tree block %llu mirror %d",
			      bytenr, mirror);
			goto out;
		}
	}

	scrub_ctx->tree_bytes_scrubbed += nodesize;
	if (csum_tree_block(fs_info, eb, 1)) {
		error("tree block %llu mirror %d checksum mismatch", bytenr,
			mirror);
		scrub_ctx->csum_errors++;
		ret = -EIO;
		goto out;
	}
	ret = check_tree_block(fs_info, eb);
	if (ret < 0) {
		error("tree block %llu mirror %d is invalid", bytenr, mirror);
		scrub_ctx->verify_errors++;
		goto out;
	}

	scrub_ctx->tree_extents_scrubbed++;
out:
	free_extent_buffer(eb);
	return ret;
}

/*
 * read_extent_data() helper
 *
 * This function will handle short read and update @scrub_ctx when read
 * error happens.
 */
static int read_extent_data_loop(struct btrfs_fs_info *fs_info,
				 struct btrfs_scrub_progress *scrub_ctx,
				 char *buf, u64 start, u64 len, int mirror)
{
	int ret = 0;
	u64 cur = 0;

	while (cur < len) {
		u64 read_len = len - cur;

		ret = read_extent_data(fs_info, buf + cur,
					start + cur, &read_len, mirror);
		if (ret < 0) {
			error("failed to read out data at bytenr %llu mirror %d",
				start + cur, mirror);
			scrub_ctx->read_errors++;
			break;
		}
		cur += read_len;
	}
	return ret;
}

/*
 * Recover all other (corrupted) mirrors for tree block.
 *
 * The method is quite simple, just read out the correct mirror specified by
 * @good_mirror and write back correct data to all other blocks
 */
static int recover_tree_mirror(struct btrfs_fs_info *fs_info,
			       struct btrfs_scrub_progress *scrub_ctx,
			       u64 start, int good_mirror)
{
	char *buf;
	u32 nodesize = fs_info->nodesize;
	int i;
	int num_copies;
	int ret;

	buf = malloc(nodesize);
	if (!buf)
		return -ENOMEM;
	ret = read_extent_data_loop(fs_info, scrub_ctx, buf, start, nodesize,
				    good_mirror);
	if (ret < 0) {
		error("failed to read tree block at bytenr %llu mirror %d",
			start, good_mirror);
		goto out;
	}

	num_copies = btrfs_num_copies(fs_info, start, nodesize);
	for (i = 0; i <= num_copies; i++) {
		if (i == good_mirror)
			continue;
		ret = write_data_to_disk(fs_info, buf, start, nodesize, i);
		if (ret < 0) {
			error("failed to write tree block at bytenr %llu mirror %d",
				start, i);
			goto out;
		}
	}
	ret = 0;
out:
	free(buf);
	return ret;
}

/*
 * Check one data mirror given by @start @len and @mirror, or @data
 * If @data is not given, try to read it from disk.
 * This function will try to read out all the data then check sum.
 *
 * If @data is given, just use the data.
 * This behavior is useful for RAID5/6 recovery code to verify recovered data.
 *
 * If @corrupt_bitmap is given, restore corrupted sector to that bitmap.
 * This is useful for mirror based profiles to recover its data.
 *
 * Return 0 if everything is OK.
 * Return <0 if something goes wrong, and @scrub_ctx accounting will be updated
 * if it's a data corruption.
 */
static int check_data_mirror(struct btrfs_fs_info *fs_info,
			     struct btrfs_scrub_progress *scrub_ctx,
			     char *data, u64 start, u64 len, int mirror,
			     unsigned long *corrupt_bitmap)
{
	u32 sectorsize = fs_info->sectorsize;
	u32 data_csum;
	u32 *csums = NULL;
	char *buf = NULL;
	int ret = 0;
	int err = 0;
	int i;
	unsigned long *csum_bitmap = NULL;

	if (!data) {
		buf = malloc(len);
		if (!buf)
			return -ENOMEM;
		ret = read_extent_data_loop(fs_info, scrub_ctx, buf, start,
					     len, mirror);
		if (ret < 0)
			goto out;
		scrub_ctx->data_bytes_scrubbed += len;
	} else {
		buf = data;
	}

	/* Alloc and Check csums */
	csums = malloc(len / sectorsize * sizeof(data_csum));
	if (!csums) {
		ret = -ENOMEM;
		goto out;
	}
	csum_bitmap = malloc(calculate_bitmap_len(len / sectorsize));
	if (!csum_bitmap) {
		ret = -ENOMEM;
		goto out;
	}

	if (corrupt_bitmap)
		memset(corrupt_bitmap, 0,
			calculate_bitmap_len(len / sectorsize));
	ret = btrfs_read_data_csums(fs_info, start, len, csums, csum_bitmap);
	if (ret < 0)
		goto out;

	for (i = 0; i < len / sectorsize; i++) {
		if (!test_bit(i, csum_bitmap)) {
			scrub_ctx->csum_discards++;
			continue;
		}

		data_csum = ~(u32)0;
		data_csum = btrfs_csum_data(buf + i * sectorsize, data_csum,
					    sectorsize);
		btrfs_csum_final(data_csum, (u8 *)&data_csum);

		if (memcmp(&data_csum, (char *)csums + i * sizeof(data_csum),
				   sizeof(data_csum))) {
			error("data at bytenr %llu mirror %d csum mismatch, have 0x%08x expect 0x%08x",
			      start + i * sectorsize, mirror, data_csum,
			      *(u32 *)((char *)csums + i * sizeof(data_csum)));
			err = 1;
			scrub_ctx->csum_errors++;
			if (corrupt_bitmap)
				set_bit(i, corrupt_bitmap);
			continue;
		}
		scrub_ctx->data_bytes_scrubbed += sectorsize;
	}
out:
	if (!data)
		free(buf);
	free(csums);
	free(csum_bitmap);

	if (!ret && err)
		return -EIO;
	return ret;
}

/* Helper to check all mirrors for a good copy */
static int has_good_mirror(unsigned long *corrupt_bitmaps[], int num_copies,
			   int bit, int *good_mirror)
{
	int found_good = 0;
	int i;

	for (i = 0; i < num_copies; i++) {
		if (!test_bit(bit, corrupt_bitmaps[i])) {
			found_good = 1;
			if (good_mirror)
				*good_mirror = i + 1;
			break;
		}
	}
	return found_good;
}

/*
 * Helper function to check @corrupt_bitmaps, to verify if it's recoverable
 * for mirror based data extent.
 *
 * Return 1 for recoverable, and 0 for not recoverable
 */
static int check_data_mirror_recoverable(struct btrfs_fs_info *fs_info,
					 u64 start, u64 len, u32 sectorsize,
					 unsigned long *corrupt_bitmaps[])
{
	int i;
	int corrupted = 0;
	int bit;
	int num_copies = btrfs_num_copies(fs_info, start, len);

	for (i = 0; i < num_copies; i++) {
		for_each_set_bit(bit, corrupt_bitmaps[i], len / sectorsize) {
			if (!has_good_mirror(corrupt_bitmaps, num_copies,
					     bit, NULL)) {
				corrupted = 1;
				goto out;
			}
		}
	}
out:
	return !corrupted;
}

/*
 * Try to recover all corrupted sectors specified by @corrupt_bitmaps,
 * by reading out good sector in other mirror.
 */
static int recover_data_mirror(struct btrfs_fs_info *fs_info,
			       struct btrfs_scrub_progress *scrub_ctx,
			       u64 start, u64 len,
			       unsigned long *corrupt_bitmaps[])
{
	char *buf;
	u32 sectorsize = fs_info->sectorsize;
	int ret = 0;
	int bit;
	int i;
	int bad_mirror;
	int num_copies;

	/* Don't bother to recover unrecoverable extents */
	if (!check_data_mirror_recoverable(fs_info, start, len,
					   sectorsize, corrupt_bitmaps))
		return -EIO;

	buf = malloc(sectorsize);
	if (!buf)
		return -ENOMEM;

	num_copies = btrfs_num_copies(fs_info, start, len);
	for (i = 0; i < num_copies; i++) {
		for_each_set_bit(bit, corrupt_bitmaps[i], BITS_PER_LONG) {
			u64 cur = start + bit * sectorsize;
			int good;

			/* Find good mirror */
			ret = has_good_mirror(corrupt_bitmaps, num_copies, bit,
					      &good);
			if (!ret) {
				error("failed to find good mirror for bytenr %llu",
					cur);
				ret = -EIO;
				goto out;
			}
			/* Read out good mirror */
			ret = read_data_from_disk(fs_info, buf, cur,
						  sectorsize, good);
			if (ret < 0) {
				error("failed to read good mirror from bytenr %llu mirror %d",
					cur, good);
				goto out;
			}
			/* Write back to all other mirrors */
			for (bad_mirror = 1; bad_mirror <= num_copies;
			     bad_mirror++) {
				if (bad_mirror == good)
					continue;
				ret = write_data_to_disk(fs_info, buf, cur,
						sectorsize, bad_mirror);
				if (ret < 0) {
					error("failed to recover mirror for bytenr %llu mirror %d",
						cur, bad_mirror);
					goto out;
				}
			}
		}
	}
out:
	free(buf);
	return ret;
}
