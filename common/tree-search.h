#ifndef __COMMON_TREE_SEARCH_H__
#define __COMMON_TREE_SEARCH_H__

#include "kerncompat.h"
#include <stdbool.h>
#include "kernel-shared/uapi/btrfs.h"

#define BTRFS_TREE_SEARCH_V2_BUF_SIZE		65536

struct btrfs_tree_search_args {
	bool use_v2;
	union {
		struct btrfs_ioctl_search_args args1;
		struct btrfs_ioctl_search_args_v2 args2;
		u8 filler[sizeof(struct btrfs_ioctl_search_args_v2) +
			  BTRFS_TREE_SEARCH_V2_BUF_SIZE];
	};
};

int btrfs_tree_search_ioctl(int fd, struct btrfs_tree_search_args *sa);

static inline struct btrfs_ioctl_search_key *btrfs_tree_search_sk(struct btrfs_tree_search_args *sa)
{
	/* Same offset for v1 and v2. */
	return &sa->args1.key;
}

static inline void *btrfs_tree_search_data(struct btrfs_tree_search_args *sa, unsigned long offset) {
	if (sa->use_v2)
		return (void *)(sa->args2.buf + offset);
	return (void *)(sa->args1.buf + offset);
}

#endif
