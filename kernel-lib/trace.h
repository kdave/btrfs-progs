#ifndef __PROGS_TRACE_H__
#define __PROGS_TRACE_H__

struct btrfs_work;
struct btrfs_fs_info;

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

#endif /* __PROGS_TRACE_H__ */
