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

/*
 * Defines and function declarations for code shared by both lowmem and
 * original mode
 */
#ifndef __BTRFS_CHECK_MODE_COMMON_H__
#define __BTRFS_CHECK_MODE_COMMON_H__

#include <sys/stat.h>
#include "kernel-shared/ctree.h"

#define FREE_SPACE_CACHE_INODE_MODE	(0100600)
/*
 * Use for tree walk to walk through trees whose leaves/nodes can be shared
 * between different trees. (Namely subvolume/fs trees)
 */
struct node_refs {
	u64 bytenr[BTRFS_MAX_LEVEL];
	u64 refs[BTRFS_MAX_LEVEL];
	int need_check[BTRFS_MAX_LEVEL];
	/* field for checking all trees */
	int checked[BTRFS_MAX_LEVEL];
	/* the corresponding extent should be marked as full backref or not */
	int full_backref[BTRFS_MAX_LEVEL];
};

enum task_position {
	TASK_ROOT_ITEMS,
	TASK_EXTENTS,
	TASK_FREE_SPACE,
	TASK_FS_ROOTS,
	TASK_CSUMS,
	TASK_ROOT_REFS,
	TASK_QGROUPS,
	TASK_NOTHING, /* has to be the last element */
};

struct task_ctx {
	int progress_enabled;
	enum task_position tp;
	time_t start_time;
	u64 item_count;

	struct task_info *info;
};

extern u64 bytes_used;
extern u64 total_csum_bytes;
extern u64 total_btree_bytes;
extern u64 total_fs_tree_bytes;
extern u64 total_extent_tree_bytes;
extern u64 btree_space_waste;
extern u64 data_bytes_allocated;
extern u64 data_bytes_referenced;
extern struct list_head duplicate_extents;
extern struct list_head delete_items;
extern int no_holes;
extern int init_extent_tree;
extern int check_data_csum;
extern struct btrfs_fs_info *gfs_info;
extern struct task_ctx ctx;
extern struct cache_tree *roots_info_cache;

static inline u8 imode_to_type(u32 imode)
{
#define S_SHIFT 12
	static unsigned char btrfs_type_by_mode[S_IFMT >> S_SHIFT] = {
		[S_IFREG >> S_SHIFT]	= BTRFS_FT_REG_FILE,
		[S_IFDIR >> S_SHIFT]	= BTRFS_FT_DIR,
		[S_IFCHR >> S_SHIFT]	= BTRFS_FT_CHRDEV,
		[S_IFBLK >> S_SHIFT]	= BTRFS_FT_BLKDEV,
		[S_IFIFO >> S_SHIFT]	= BTRFS_FT_FIFO,
		[S_IFSOCK >> S_SHIFT]	= BTRFS_FT_SOCK,
		[S_IFLNK >> S_SHIFT]	= BTRFS_FT_SYMLINK,
	};

	return btrfs_type_by_mode[(imode & S_IFMT) >> S_SHIFT];
#undef S_SHIFT
}

static inline int fs_root_objectid(u64 objectid)
{
	if (objectid == BTRFS_TREE_RELOC_OBJECTID ||
	    objectid == BTRFS_DATA_RELOC_TREE_OBJECTID)
		return 1;
	return is_fstree(objectid);
}

int check_prealloc_extent_written(u64 disk_bytenr, u64 num_bytes);
int count_csum_range(u64 start, u64 len, u64 *found);
int insert_inode_item(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, u64 ino, u64 size,
		      u64 nbytes, u64 nlink, u32 mode);
int link_inode_to_lostfound(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root,
			    struct btrfs_path *path,
			    u64 ino, char *namebuf, u32 name_len,
			    u8 filetype, u64 *ref_count);
void check_dev_size_alignment(u64 devid, u64 total_bytes, u32 sectorsize);
void reada_walk_down(struct btrfs_root *root, struct extent_buffer *node,
		     int slot);
int check_child_node(struct extent_buffer *parent, int slot,
		     struct extent_buffer *child);
void reset_cached_block_groups(void);
int pin_metadata_blocks(void);
int exclude_metadata_blocks(void);
void cleanup_excluded_extents(void);
int delete_corrupted_dir_item(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root,
			      struct btrfs_key *di_key, char *namebuf,
			      u32 namelen);
int detect_imode(struct btrfs_root *root, struct btrfs_path *path,
		 u32 *imode_ret);
int reset_imode(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		struct btrfs_path *path, u64 ino, u32 mode);
int repair_imode_common(struct btrfs_root *root, struct btrfs_path *path);
int check_repair_free_space_inode(struct btrfs_path *path);

/*
 * Check if the inode mode @imode is valid
 *
 * This check focuses on S_FTMT bits and unused bits.
 * Sticky/setuid/setgid and regular owner/group/other bits won't cause
 * any problem.
 */
static inline bool is_valid_imode(u32 imode)
{
	if (imode & ~(S_IFMT | 07777))
		return false;

	/*
	 * S_IFMT is not bitmap, nor pure numbering sequence. Need per valid
	 * number check.
	 */
	imode &= S_IFMT;
	if (imode != S_IFDIR && imode != S_IFCHR && imode != S_IFBLK &&
	    imode != S_IFREG && imode != S_IFIFO && imode != S_IFLNK &&
	    imode != S_IFSOCK)
		return false;
	return true;
}

int recow_extent_buffer(struct btrfs_root *root, struct extent_buffer *eb);

static inline u32 btrfs_type_to_imode(u8 type)
{
	static u32 imode_by_btrfs_type[] = {
		[BTRFS_FT_REG_FILE]	= S_IFREG,
		[BTRFS_FT_DIR]		= S_IFDIR,
		[BTRFS_FT_CHRDEV]	= S_IFCHR,
		[BTRFS_FT_BLKDEV]	= S_IFBLK,
		[BTRFS_FT_FIFO]		= S_IFIFO,
		[BTRFS_FT_SOCK]		= S_IFSOCK,
		[BTRFS_FT_SYMLINK]	= S_IFLNK,
	};

	return imode_by_btrfs_type[(type)];
}

int get_extent_item_generation(u64 bytenr, u64 *gen_ret);

/*
 * Check tree block alignment for future subpage support.
 *
 * For subpage support, either nodesize is smaller than PAGE_SIZE, then tree
 * block should not cross page boundary. (A)
 * Or nodesize >= PAGE_SIZE, then it should be page aligned. (B)
 *
 * But here we have no idea the PAGE_SIZE could be, so here we play safe by
 * requiring all tree blocks to be nodesize aligned.
 *
 * For 4K page size system, it always meets condition (B), thus we don't need
 * to bother that much.
 */
static inline void btrfs_check_subpage_eb_alignment(struct btrfs_fs_info *info,
						    u64 start, u32 len)
{
	if (!IS_ALIGNED(start, info->nodesize))
		warning(
"tree block [%llu, %llu) is not nodesize aligned, may cause problem for 64K page system",
			start, start + len);
}

int repair_dev_item_bytes_used(struct btrfs_fs_info *fs_info,
			       u64 devid, u64 bytes_used_expected);

int fill_csum_tree(struct btrfs_trans_handle *trans, bool search_fs_tree);

int check_and_repair_super_num_devs(struct btrfs_fs_info *fs_info);

#endif
