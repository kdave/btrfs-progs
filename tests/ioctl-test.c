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
#include <stdio.h>
#include <stdlib.h>

#include "ioctl.h"
#include "kernel-shared/ctree.h"

#define LIST_32_COMPAT				\
	ONE(BTRFS_IOC_SET_RECEIVED_SUBVOL_32)

#define LIST_64_COMPAT				\
	ONE(BTRFS_IOC_SEND_64)

#define LIST_BASE				\
	ONE(BTRFS_IOC_SNAP_CREATE)		\
	ONE(BTRFS_IOC_DEFRAG)			\
	ONE(BTRFS_IOC_RESIZE)			\
	ONE(BTRFS_IOC_SCAN_DEV)			\
	ONE(BTRFS_IOC_SYNC)			\
	ONE(BTRFS_IOC_CLONE)			\
	ONE(BTRFS_IOC_ADD_DEV)			\
	ONE(BTRFS_IOC_RM_DEV)			\
	ONE(BTRFS_IOC_BALANCE)			\
	ONE(BTRFS_IOC_CLONE_RANGE)		\
	ONE(BTRFS_IOC_SUBVOL_CREATE)		\
	ONE(BTRFS_IOC_SNAP_DESTROY)		\
	ONE(BTRFS_IOC_DEFRAG_RANGE)		\
	ONE(BTRFS_IOC_TREE_SEARCH)		\
	ONE(BTRFS_IOC_TREE_SEARCH_V2)		\
	ONE(BTRFS_IOC_INO_LOOKUP)		\
	ONE(BTRFS_IOC_DEFAULT_SUBVOL)		\
	ONE(BTRFS_IOC_SPACE_INFO)		\
	ONE(BTRFS_IOC_START_SYNC)		\
	ONE(BTRFS_IOC_WAIT_SYNC)		\
	ONE(BTRFS_IOC_SNAP_CREATE_V2)		\
	ONE(BTRFS_IOC_SUBVOL_CREATE_V2)		\
	ONE(BTRFS_IOC_SUBVOL_GETFLAGS)		\
	ONE(BTRFS_IOC_SUBVOL_SETFLAGS)		\
	ONE(BTRFS_IOC_SCRUB)			\
	ONE(BTRFS_IOC_SCRUB_CANCEL)		\
	ONE(BTRFS_IOC_SCRUB_PROGRESS)		\
	ONE(BTRFS_IOC_DEV_INFO)			\
	ONE(BTRFS_IOC_FS_INFO)			\
	ONE(BTRFS_IOC_BALANCE_V2)		\
	ONE(BTRFS_IOC_BALANCE_CTL)		\
	ONE(BTRFS_IOC_BALANCE_PROGRESS)		\
	ONE(BTRFS_IOC_INO_PATHS)		\
	ONE(BTRFS_IOC_LOGICAL_INO)		\
	ONE(BTRFS_IOC_SET_RECEIVED_SUBVOL)	\
	ONE(BTRFS_IOC_SEND)			\
	ONE(BTRFS_IOC_DEVICES_READY)		\
	ONE(BTRFS_IOC_QUOTA_CTL)		\
	ONE(BTRFS_IOC_QGROUP_ASSIGN)		\
	ONE(BTRFS_IOC_QGROUP_CREATE)		\
	ONE(BTRFS_IOC_QGROUP_LIMIT)		\
	ONE(BTRFS_IOC_QUOTA_RESCAN)		\
	ONE(BTRFS_IOC_QUOTA_RESCAN_STATUS)	\
	ONE(BTRFS_IOC_QUOTA_RESCAN_WAIT)	\
	ONE(BTRFS_IOC_GET_FSLABEL)		\
	ONE(BTRFS_IOC_SET_FSLABEL)		\
	ONE(BTRFS_IOC_GET_DEV_STATS)		\
	ONE(BTRFS_IOC_DEV_REPLACE)		\
	ONE(BTRFS_IOC_FILE_EXTENT_SAME)		\
	ONE(BTRFS_IOC_GET_FEATURES)		\
	ONE(BTRFS_IOC_SET_FEATURES)		\
	ONE(BTRFS_IOC_GET_SUPPORTED_FEATURES)	\
	ONE(BTRFS_IOC_RM_DEV_V2)		\
	ONE(BTRFS_IOC_LOGICAL_INO_V2)

#define LIST					\
	LIST_BASE				\
	LIST_32_COMPAT				\
	LIST_64_COMPAT

struct ioctl_number {
	unsigned long defined;
	unsigned long expected;
};

static struct ioctl_number expected_list[] = {
	{ BTRFS_IOC_SNAP_CREATE,                    0x0050009401 },
	{ BTRFS_IOC_DEFRAG,                         0x0050009402 },
	{ BTRFS_IOC_RESIZE,                         0x0050009403 },
	{ BTRFS_IOC_SCAN_DEV,                       0x0050009404 },
	{ BTRFS_IOC_SYNC,                           0x0000009408 },
	{ BTRFS_IOC_CLONE,                          0x0040049409 },
	{ BTRFS_IOC_ADD_DEV,                        0x005000940a },
	{ BTRFS_IOC_RM_DEV,                         0x005000940b },
	{ BTRFS_IOC_BALANCE,                        0x005000940c },
	{ BTRFS_IOC_CLONE_RANGE,                    0x004020940d },
	{ BTRFS_IOC_SUBVOL_CREATE,                  0x005000940e },
	{ BTRFS_IOC_SNAP_DESTROY,                   0x005000940f },
	{ BTRFS_IOC_DEFRAG_RANGE,                   0x0040309410 },
	{ BTRFS_IOC_TREE_SEARCH,                    0x00d0009411 },
	{ BTRFS_IOC_TREE_SEARCH_V2,                 0x00c0709411 },
	{ BTRFS_IOC_INO_LOOKUP,                     0x00d0009412 },
	{ BTRFS_IOC_DEFAULT_SUBVOL,                 0x0040089413 },
	{ BTRFS_IOC_SPACE_INFO,                     0x00c0109414 },
	{ BTRFS_IOC_START_SYNC,                     0x0080089418 },
	{ BTRFS_IOC_WAIT_SYNC,                      0x0040089416 },
	{ BTRFS_IOC_SNAP_CREATE_V2,                 0x0050009417 },
	{ BTRFS_IOC_SUBVOL_CREATE_V2,               0x0050009418 },
	{ BTRFS_IOC_SUBVOL_GETFLAGS,                0x0080089419 },
	{ BTRFS_IOC_SUBVOL_SETFLAGS,                0x004008941a },
	{ BTRFS_IOC_SCRUB,                          0x00c400941b },
	{ BTRFS_IOC_SCRUB_CANCEL,                   0x000000941c },
	{ BTRFS_IOC_SCRUB_PROGRESS,                 0x00c400941d },
	{ BTRFS_IOC_DEV_INFO,                       0x00d000941e },
	{ BTRFS_IOC_FS_INFO,                        0x008400941f },
	{ BTRFS_IOC_BALANCE_V2,                     0x00c4009420 },
	{ BTRFS_IOC_BALANCE_CTL,                    0x0040049421 },
	{ BTRFS_IOC_BALANCE_PROGRESS,               0x0084009422 },
	{ BTRFS_IOC_INO_PATHS,                      0x00c0389423 },
	{ BTRFS_IOC_LOGICAL_INO,                    0x00c0389424 },
	{ BTRFS_IOC_SET_RECEIVED_SUBVOL,            0x00c0c89425 },
#ifdef BTRFS_IOC_SET_RECEIVED_SUBVOL_32_COMPAT_DEFINED
	{ BTRFS_IOC_SET_RECEIVED_SUBVOL_32,         0x00c0c09425 },
#endif
#if BITS_PER_LONG == 32
	{ BTRFS_IOC_SEND,                           0x0040449426 },
#elif BITS_PER_LONG == 64
	{ BTRFS_IOC_SEND,                           0x0040489426 },
#endif
#ifdef BTRFS_IOC_SEND_64_COMPAT_DEFINED
	{ BTRFS_IOC_SEND_64,                        0x0040489426 },
#endif
	{ BTRFS_IOC_DEVICES_READY,                  0x0090009427 },
	{ BTRFS_IOC_QUOTA_CTL,                      0x00c0109428 },
	{ BTRFS_IOC_QGROUP_ASSIGN,                  0x0040189429 },
	{ BTRFS_IOC_QGROUP_CREATE,                  0x004010942a },
	{ BTRFS_IOC_QGROUP_LIMIT,                   0x008030942b },
	{ BTRFS_IOC_QUOTA_RESCAN,                   0x004040942c },
	{ BTRFS_IOC_QUOTA_RESCAN_STATUS,            0x008040942d },
	{ BTRFS_IOC_QUOTA_RESCAN_WAIT,              0x000000942e },
	{ BTRFS_IOC_GET_FSLABEL,                    0x0081009431 },
	{ BTRFS_IOC_SET_FSLABEL,                    0x0041009432 },
	{ BTRFS_IOC_GET_DEV_STATS,                  0x00c4089434 },
	{ BTRFS_IOC_DEV_REPLACE,                    0x00ca289435 },
	{ BTRFS_IOC_FILE_EXTENT_SAME,               0x00c0189436 },
	{ BTRFS_IOC_GET_FEATURES,                   0x0080189439 },
	{ BTRFS_IOC_SET_FEATURES,                   0x0040309439 },
	{ BTRFS_IOC_GET_SUPPORTED_FEATURES,         0x0080489439 },
	{ BTRFS_IOC_RM_DEV_V2,                      0x005000943a },
	{ BTRFS_IOC_LOGICAL_INO_V2,                 0x00c038943b },
};

static struct btrfs_ioctl_vol_args used_vol_args __attribute__((used));
static struct btrfs_ioctl_vol_args_v2 used_vol_args2 __attribute__((used));
static struct btrfs_ioctl_clone_range_args used_clone_args __attribute__((used));
static struct btrfs_ioctl_defrag_range_args used_defrag_args __attribute__((used));
static struct btrfs_ioctl_search_args used_search_args __attribute__((used));
static struct btrfs_ioctl_search_args_v2 used_search_args2 __attribute__((used));
static struct btrfs_ioctl_ino_lookup_args used_ino_lookup __attribute__((used));
static struct btrfs_ioctl_space_args used_space_args __attribute__((used));
static struct btrfs_ioctl_scrub_args used_scrub_args __attribute__((used));
static struct btrfs_ioctl_dev_info_args used_dev_info_args __attribute__((used));
static struct btrfs_ioctl_fs_info_args used_fs_info_args __attribute__((used));
static struct btrfs_ioctl_balance_args used_balance_args __attribute__((used));
static struct btrfs_ioctl_ino_path_args used_path_args __attribute__((used));
static struct btrfs_ioctl_logical_ino_args used_logical_args __attribute__((used));
static struct btrfs_ioctl_received_subvol_args used_received_args __attribute__((used));
#ifdef BTRFS_IOC_SET_RECEIVED_SUBVOL_32_COMPAT_DEFINED
static struct btrfs_ioctl_received_subvol_args_32 used_received_args32 __attribute__((used));
#endif
static struct btrfs_ioctl_send_args used_send_args __attribute__((used));
#ifdef BTRFS_IOC_SEND_64_COMPAT_DEFINED
static struct btrfs_ioctl_send_args_64 used_send_args64 __attribute__((used));
#endif
static struct btrfs_ioctl_quota_ctl_args used_qgctl_args __attribute__((used));
static struct btrfs_ioctl_qgroup_assign_args used_qgassign_args __attribute__((used));
static struct btrfs_ioctl_qgroup_create_args used_qgcreate_args __attribute__((used));
static struct btrfs_ioctl_qgroup_limit_args used_qglimit_args __attribute__((used));
static struct btrfs_ioctl_quota_rescan_args used_qgrescan_args __attribute__((used));
static struct btrfs_ioctl_get_dev_stats used_dev_stats_args __attribute__((used));
static struct btrfs_ioctl_dev_replace_args used_replace_args __attribute__((used));
static struct btrfs_ioctl_same_args used_same_args __attribute__((used));
static struct btrfs_ioctl_feature_flags used_feature_flags __attribute__((used));

const char* value_to_string(unsigned long num)
{
#define ONE(x)	case x: return #x;
	switch (num) {
	LIST_BASE
	}

	switch (num) {
	LIST_32_COMPAT
	}

	switch (num) {
	LIST_64_COMPAT
	}
#undef ONE
	return "UNKNOWN";
}

int main(int ac, char **av)
{
	int i;
	int errors = 0;

	value_to_string(random());

	printf("Sizeof long long:  %zu\n", sizeof(unsigned long long));
	printf("Sizeof long:       %zu\n", sizeof(unsigned long));
	printf("Sizeof pointer:    %zu\n", sizeof(void*));
	printf("Alignof long long: %zu\n", __alignof__(unsigned long long));
	printf("Alignof long:      %zu\n", __alignof__(unsigned long));
	printf("Alignof pointer:   %zu\n", __alignof__(void*));
	printf("Raw ioctl numbers:\n");

#define ONE(n)	printf("%-38s   0x%010lx\n", #n, (unsigned long)n);
	LIST
#undef ONE

	for (i = 0; i < ARRAY_SIZE(expected_list); i++) {
		if (expected_list[i].defined != expected_list[i].expected) {
			printf("ERROR: wrong value for %s, defined=0x%lx expected=0x%lx\n",
					value_to_string(expected_list[i].defined),
					expected_list[i].defined,
					expected_list[i].expected);
			errors++;
		}
	}

	if (!errors) {
		printf("All ok\n");
	} else {
		printf("Found %d errors in definitions\n", errors);
	}

	return !!errors;
}

