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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/transaction.h"
#include "common/messages.h"
#include "common/internal.h"
#include "tune/tune.h"

int btrfs_change_csum_type(struct btrfs_fs_info *fs_info, u16 new_csum_type)
{
	/* Phase 0, check conflicting features. */

	/*
	 * Phase 1, generate new data csums.
	 *
	 * The new data csums would have a different key objectid, and there
	 * will be a temporary item in root tree to indicate the new checksum
	 * algo.
	 */

	/* Phase 2, delete the old data csums. */

	/* Phase 3, change the new csum key objectid */

	/*
	 * Phase 4, change the csums for metadata.
	 *
	 * This has to be done in-place, as we don't have a good method
	 * like relocation in progs.
	 * Thus we have to support reading a tree block with either csum.
	 */
	return -EOPNOTSUPP;
}
