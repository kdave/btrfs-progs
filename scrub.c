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
#include "kernel-lib/raid56.h"

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
		for_each_set_bit(bit, corrupt_bitmaps[i], len / sectorsize) {
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

/* Btrfs only supports up to 2 copies of data, yet */
#define BTRFS_MAX_COPIES	2

/*
 * Check all copies of range @start, @len.
 * Caller must ensure the range is covered by EXTENT_ITEM/METADATA_ITEM
 * specified by leaf of @path.
 * And @start, @len must be a subset of the EXTENT_ITEM/METADATA_ITEM.
 *
 * Return 0 if the range is all OK or recovered or recoverable.
 * Return <0 if the range can't be recoverable.
 */
static int scrub_one_extent(struct btrfs_fs_info *fs_info,
			    struct btrfs_scrub_progress *scrub_ctx,
			    struct btrfs_path *path, u64 start, u64 len,
			    int write)
{
	struct btrfs_key key;
	struct btrfs_extent_item *ei;
	struct extent_buffer *leaf = path->nodes[0];
	u32 sectorsize = fs_info->sectorsize;
	unsigned long *corrupt_bitmaps[BTRFS_MAX_COPIES] = { NULL };
	int slot = path->slots[0];
	int num_copies;
	int meta_corrupted = 0;
	int meta_good_mirror = 0;
	int data_bad_mirror = 0;
	u64 extent_start;
	u64 extent_len;
	int metadata = 0;
	int i;
	int ret = 0;

	btrfs_item_key_to_cpu(leaf, &key, slot);
	if (key.type != BTRFS_METADATA_ITEM_KEY &&
	    key.type != BTRFS_EXTENT_ITEM_KEY)
		goto invalid_arg;

	extent_start = key.objectid;
	if (key.type == BTRFS_METADATA_ITEM_KEY) {
		extent_len = fs_info->nodesize;
		metadata = 1;
	} else {
		extent_len = key.offset;
		ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);
		if (btrfs_extent_flags(leaf, ei) & BTRFS_EXTENT_FLAG_TREE_BLOCK)
			metadata = 1;
	}
	if (start >= extent_start + extent_len ||
	    start + len <= extent_start)
		goto invalid_arg;

	for (i = 0; i < BTRFS_MAX_COPIES; i++) {
		corrupt_bitmaps[i] = malloc(
				calculate_bitmap_len(len / sectorsize));
		if (!corrupt_bitmaps[i])
			goto out;
	}
	num_copies = btrfs_num_copies(fs_info, start, len);
	for (i = 1; i <= num_copies; i++) {
		if (metadata) {
			ret = check_tree_mirror(fs_info, scrub_ctx,
					NULL, extent_start, i);
			scrub_ctx->tree_extents_scrubbed++;
			if (ret < 0)
				meta_corrupted++;
			else
				meta_good_mirror = i;
		} else {
			ret = check_data_mirror(fs_info, scrub_ctx, NULL, start,
						len, i, corrupt_bitmaps[i - 1]);
			scrub_ctx->data_extents_scrubbed++;
		}
	}

	/* Metadata recover and report */
	if (metadata) {
		if (!meta_corrupted) {
			goto out;
		} else if (meta_corrupted && meta_corrupted < num_copies) {
			if (write) {
				ret = recover_tree_mirror(fs_info, scrub_ctx,
						start, meta_good_mirror);
				if (ret < 0) {
					error("failed to recover tree block at bytenr %llu",
						start);
					goto out;
				}
				printf("extent %llu len %llu REPAIRED: has corrupted mirror, repaired\n",
					start, len);
				goto out;
			}
			printf("extent %llu len %llu RECOVERABLE: has corrupted mirror, but is recoverable\n",
				start, len);
			goto out;
		} else {
			error("extent %llu len %llu CORRUPTED: all mirror(s) corrupted, can't be recovered",
				start, len);
			ret = -EIO;
			goto out;
		}
	}
	/* Data recover and report */
	for (i = 0; i < num_copies; i++) {
		if (find_first_bit(corrupt_bitmaps[i], len / sectorsize) >=
		    len / sectorsize)
			continue;
		data_bad_mirror = i + 1;
	}
	/* All data sectors are good */
	if (!data_bad_mirror) {
		ret = 0;
		goto out;
	}

	if (check_data_mirror_recoverable(fs_info, start, len,
					  sectorsize, corrupt_bitmaps)) {
		if (write) {
			ret = recover_data_mirror(fs_info, scrub_ctx, start,
						  len, corrupt_bitmaps);
			if (ret < 0) {
				error("failed to recover data extent at bytenr %llu len %llu",
					start, len);
				goto out;
			}
			printf("extent %llu len %llu REPARIED: has corrupted mirror, repaired\n",
				start, len);
			goto out;
		}
		printf("extent %llu len %llu RECOVERABLE: has corrupted mirror, recoverable\n",
			start, len);
		goto out;
	}
	error("extent %llu len %llu CORRUPTED, all mirror(s) corrupted, can't be repaired",
		start, len);
	ret = -EIO;
out:
	for (i = 0; i < BTRFS_MAX_COPIES; i++)
		kfree(corrupt_bitmaps[i]);
	return ret;

invalid_arg:
	error("invalid parameter for %s", __func__);
	return -EINVAL;
}

/*
 * Scrub one full data stripe of RAID5/6.
 * This means it will check any data/metadata extent in the data stripe
 * spcified by @stripe and @stripe_len
 *
 * This function will only *CHECK* if the data stripe has any corruption.
 * Won't repair at this function.
 *
 * Return 0 if the full stripe is OK.
 * Return <0 if any error is found.
 * Note: Missing csum is not counted as error (NODATACSUM is valid)
 */
static int scrub_one_data_stripe(struct btrfs_fs_info *fs_info,
				 struct btrfs_scrub_progress *scrub_ctx,
				 struct scrub_stripe *stripe, u32 stripe_len)
{
	struct btrfs_path *path;
	struct btrfs_root *extent_root = fs_info->extent_root;
	struct btrfs_key key;
	u64 extent_start;
	u64 extent_len;
	u64 orig_csum_discards;
	int ret;

	if (!is_data_stripe(stripe))
		return -EINVAL;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = stripe->logical + stripe_len;
	key.offset = 0;
	key.type = 0;

	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	while (1) {
		struct btrfs_extent_item *ei;
		struct extent_buffer *eb;
		char *data;
		int slot;
		int metadata = 0;
		u64 check_start;
		u64 check_len;

		ret = btrfs_previous_extent_item(extent_root, path, 0);
		if (ret > 0) {
			ret = 0;
			goto out;
		}
		if (ret < 0)
			goto out;
		eb = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(eb, &key, slot);
		extent_start = key.objectid;
		ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);

		/* tree block scrub */
		if (key.type == BTRFS_METADATA_ITEM_KEY ||
		    btrfs_extent_flags(eb, ei) & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
			extent_len = extent_root->fs_info->nodesize;
			metadata = 1;
		} else {
			extent_len = key.offset;
			metadata = 0;
		}

		/* Current extent is out of our range, loop comes to end */
		if (extent_start + extent_len <= stripe->logical)
			break;

		if (metadata) {
			/*
			 * Check crossing stripe first, which can't be scrubbed
			 */
			if (check_crossing_stripes(fs_info, extent_start,
					extent_root->fs_info->nodesize)) {
				error("tree block at %llu is crossing stripe boundary, unable to scrub",
					extent_start);
				ret = -EIO;
				goto out;
			}
			data = stripe->data + extent_start - stripe->logical;
			ret = check_tree_mirror(fs_info, scrub_ctx,
						data, extent_start, 0);
			/* Any csum/verify error means the stripe is screwed */
			if (ret < 0) {
				stripe->csum_mismatch = 1;
				ret = -EIO;
				goto out;
			}
			ret = 0;
			continue;
		}
		/* Restrict the extent range to fit stripe range */
		check_start = max(extent_start, stripe->logical);
		check_len = min(extent_start + extent_len, stripe->logical +
				stripe_len) - check_start;

		/* Record original csum_discards to detect missing csum case */
		orig_csum_discards = scrub_ctx->csum_discards;

		data = stripe->data + check_start - stripe->logical;
		ret = check_data_mirror(fs_info, scrub_ctx, data, check_start,
					check_len, 0, NULL);
		/* Csum mismatch, no need to continue anyway*/
		if (ret < 0) {
			stripe->csum_mismatch = 1;
			goto out;
		}
		/* Check if there is any missing csum for data */
		if (scrub_ctx->csum_discards != orig_csum_discards)
			stripe->csum_missing = 1;
		/*
		 * Only increase data_extents_scrubbed if we are scrubbing the
		 * tailing part of the data extent
		 */
		if (extent_start + extent_len <= stripe->logical + stripe_len)
			scrub_ctx->data_extents_scrubbed++;
		ret = 0;
	}
out:
	btrfs_free_path(path);
	return ret;
}

/*
 * Verify parities for RAID56
 * Caller must fill @fstripe before calling this function
 *
 * Return 0 for parities matches.
 * Return >0 for P or Q mismatch
 * Return <0 for fatal error
 */
static int verify_parities(struct btrfs_fs_info *fs_info,
			   struct btrfs_scrub_progress *scrub_ctx,
			   struct scrub_full_stripe *fstripe)
{
	void **ptrs;
	void *ondisk_p = NULL;
	void *ondisk_q = NULL;
	void *buf_p;
	void *buf_q;
	int nr_stripes = fstripe->nr_stripes;
	int stripe_len = BTRFS_STRIPE_LEN;
	int i;
	int ret = 0;

	ptrs = malloc(sizeof(void *) * fstripe->nr_stripes);
	buf_p = malloc(fstripe->stripe_len);
	buf_q = malloc(fstripe->stripe_len);
	if (!ptrs || !buf_p || !buf_q) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < fstripe->nr_stripes; i++) {
		struct scrub_stripe *stripe = &fstripe->stripes[i];

		if (stripe->logical == BTRFS_RAID5_P_STRIPE) {
			ondisk_p = stripe->data;
			ptrs[i] = buf_p;
			continue;
		} else if (stripe->logical == BTRFS_RAID6_Q_STRIPE) {
			ondisk_q = stripe->data;
			ptrs[i] = buf_q;
			continue;
		} else {
			ptrs[i] = stripe->data;
			continue;
		}
	}
	/* RAID6 */
	if (ondisk_q) {
		raid6_gen_syndrome(nr_stripes, stripe_len, ptrs);

		if (memcmp(ondisk_q, ptrs[nr_stripes - 1], stripe_len) != 0 ||
		    memcmp(ondisk_p, ptrs[nr_stripes - 2], stripe_len))
			ret = 1;
	} else {
		ret = raid5_gen_result(nr_stripes, stripe_len, nr_stripes - 1,
					ptrs);
		if (ret < 0)
			goto out;
		if (memcmp(ondisk_p, ptrs[nr_stripes - 1], stripe_len) != 0)
			ret = 1;
	}
out:
	free(buf_p);
	free(buf_q);
	free(ptrs);
	return ret;
}

/*
 * Try to recover data stripe from P or Q stripe
 *
 * Return >0 if it can't be require any more.
 * Return 0 for successful repair or no need to repair at all
 * Return <0 for fatal error
 */
static int recover_from_parities(struct btrfs_fs_info *fs_info,
				  struct btrfs_scrub_progress *scrub_ctx,
				  struct scrub_full_stripe *fstripe)
{
	void **ptrs;
	int nr_stripes = fstripe->nr_stripes;
	int stripe_len = BTRFS_STRIPE_LEN;
	int max_tolerance;
	int i;
	int ret;

	/* No need to recover */
	if (!fstripe->nr_corrupted_stripes)
		return 0;

	/* Already recovered once, no more chance */
	if (fstripe->recovered)
		return 1;

	if (fstripe->bg_type & BTRFS_BLOCK_GROUP_RAID5)
		max_tolerance = 1;
	else
		max_tolerance = 2;

	/* Out of repair */
	if (fstripe->nr_corrupted_stripes > max_tolerance)
		return 1;

	ptrs = malloc(sizeof(void *) * fstripe->nr_stripes);
	if (!ptrs)
		return -ENOMEM;

	/* Construct ptrs */
	for (i = 0; i < nr_stripes; i++)
		ptrs[i] = fstripe->stripes[i].data;

	ret = raid56_recov(nr_stripes, stripe_len, fstripe->bg_type,
			fstripe->corrupted_index[0],
			fstripe->corrupted_index[1], ptrs);
	fstripe->recovered = 1;
	free(ptrs);
	return ret;
}
