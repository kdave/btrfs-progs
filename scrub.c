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
