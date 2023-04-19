#ifndef __PROGS_TRACE_H__
#define __PROGS_TRACE_H__

struct btrfs_work;
struct btrfs_fs_info;
struct extent_state;
struct extent_io_tree;

static inline void trace_btrfs_workqueue_alloc(void *ret, const char *name)
{
}

static inline void trace_btrfs_ordered_sched(struct btrfs_work *work)
{
}

static inline void trace_btrfs_all_work_done(struct btrfs_fs_info *fs_info,
					     struct btrfs_work *work)
{
}

static inline void trace_btrfs_work_sched(struct btrfs_work *work)
{
}

static inline void trace_btrfs_work_queued(struct btrfs_work *work)
{
}

static inline void trace_btrfs_workqueue_destroy(void *wq)
{
}

static inline void trace_alloc_extent_state(struct extent_state *state,
					    gfp_t mask, unsigned long ip)
{
}

static inline void trace_free_extent_state(struct extent_state *state,
					   unsigned long ip)
{
}

static inline void trace_btrfs_clear_extent_bit(struct extent_io_tree *tree,
						u64 start, u64 end, u32 bits)
{
}

static inline void trace_btrfs_set_extent_bit(struct extent_io_tree *tree,
					      u64 start, u64 end, u32 bits)
{
}

static inline void trace_btrfs_convert_extent_bit(struct extent_io_tree *tree,
						  u64 start, u64 end, u32 bits,
						  u32 clear_bits)
{
}

#endif /* __PROGS_TRACE_H__ */
