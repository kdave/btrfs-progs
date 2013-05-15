/*
 * Copyright (C) 2011 STRATO.  All rights reserved.
 *
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

#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <poll.h>
#include <sys/file.h>
#include <uuid/uuid.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>

#include "ctree.h"
#include "ioctl.h"
#include "utils.h"
#include "volumes.h"
#include "disk-io.h"

#include "commands.h"

static const char * const scrub_cmd_group_usage[] = {
	"btrfs scrub <command> [options] <path>|<device>",
	NULL
};

#define SCRUB_DATA_FILE "/var/lib/btrfs/scrub.status"
#define SCRUB_PROGRESS_SOCKET_PATH "/var/lib/btrfs/scrub.progress"
#define SCRUB_FILE_VERSION_PREFIX "scrub status"
#define SCRUB_FILE_VERSION "1"

struct scrub_stats {
	time_t t_start;
	time_t t_resumed;
	u64 duration;
	u64 finished;
	u64 canceled;
};

/* TBD: replace with #include "linux/ioprio.h" in some years */
#if !defined (IOPRIO_H)
#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_CLASS_SHIFT 13
#define IOPRIO_PRIO_VALUE(class, data) \
		(((class) << IOPRIO_CLASS_SHIFT) | (data))
#define IOPRIO_CLASS_IDLE 3
#endif

struct scrub_progress {
	struct btrfs_ioctl_scrub_args scrub_args;
	int fd;
	int ret;
	int skip;
	struct scrub_stats stats;
	struct scrub_file_record *resumed;
	int ioctl_errno;
	pthread_mutex_t progress_mutex;
	int ioprio_class;
	int ioprio_classdata;
};

struct scrub_file_record {
	u8 fsid[BTRFS_FSID_SIZE];
	u64 devid;
	struct scrub_stats stats;
	struct btrfs_scrub_progress p;
};

struct scrub_progress_cycle {
	int fdmnt;
	int prg_fd;
	int do_record;
	struct btrfs_ioctl_fs_info_args *fi;
	struct scrub_progress *progress;
	struct scrub_progress *shared_progress;
	pthread_mutex_t *write_mutex;
};

struct scrub_fs_stat {
	struct btrfs_scrub_progress p;
	struct scrub_stats s;
	int i;
};

static void print_scrub_full(struct btrfs_scrub_progress *sp)
{
	printf("\tdata_extents_scrubbed: %lld\n", sp->data_extents_scrubbed);
	printf("\ttree_extents_scrubbed: %lld\n", sp->tree_extents_scrubbed);
	printf("\tdata_bytes_scrubbed: %lld\n", sp->data_bytes_scrubbed);
	printf("\ttree_bytes_scrubbed: %lld\n", sp->tree_bytes_scrubbed);
	printf("\tread_errors: %lld\n", sp->read_errors);
	printf("\tcsum_errors: %lld\n", sp->csum_errors);
	printf("\tverify_errors: %lld\n", sp->verify_errors);
	printf("\tno_csum: %lld\n", sp->no_csum);
	printf("\tcsum_discards: %lld\n", sp->csum_discards);
	printf("\tsuper_errors: %lld\n", sp->super_errors);
	printf("\tmalloc_errors: %lld\n", sp->malloc_errors);
	printf("\tuncorrectable_errors: %lld\n", sp->uncorrectable_errors);
	printf("\tunverified_errors: %lld\n", sp->unverified_errors);
	printf("\tcorrected_errors: %lld\n", sp->corrected_errors);
	printf("\tlast_physical: %lld\n", sp->last_physical);
}

#define ERR(test, ...) do {			\
	if (test)				\
		fprintf(stderr, __VA_ARGS__);	\
} while (0)

#define PRINT_SCRUB_ERROR(test, desc) do {	\
	if (test)				\
		printf(" %s=%llu", desc, test);	\
} while (0)

static void print_scrub_summary(struct btrfs_scrub_progress *p)
{
	u64 err_cnt;
	u64 err_cnt2;
	char *bytes;

	err_cnt = p->read_errors +
			p->csum_errors +
			p->verify_errors +
			p->super_errors;

	err_cnt2 = p->corrected_errors + p->uncorrectable_errors;

	if (p->malloc_errors)
		printf("*** WARNING: memory allocation failed while scrubbing. "
		       "results may be inaccurate\n");
	bytes = pretty_sizes(p->data_bytes_scrubbed + p->tree_bytes_scrubbed);
	printf("\ttotal bytes scrubbed: %s with %llu errors\n", bytes,
		max(err_cnt, err_cnt2));
	free(bytes);
	if (err_cnt || err_cnt2) {
		printf("\terror details:");
		PRINT_SCRUB_ERROR(p->read_errors, "read");
		PRINT_SCRUB_ERROR(p->super_errors, "super");
		PRINT_SCRUB_ERROR(p->verify_errors, "verify");
		PRINT_SCRUB_ERROR(p->csum_errors, "csum");
		printf("\n");
		printf("\tcorrected errors: %llu, uncorrectable errors: %llu, "
			"unverified errors: %llu\n", p->corrected_errors,
			p->uncorrectable_errors, p->unverified_errors);
	}
}

#define _SCRUB_FS_STAT(p, name, fs_stat) do {	\
	fs_stat->p.name += p->name;		\
} while (0)

#define _SCRUB_FS_STAT_MIN(ss, name, fs_stat)	\
do {						\
	if (fs_stat->s.name > ss->name) {	\
		fs_stat->s.name = ss->name;	\
	}					\
} while (0)

#define _SCRUB_FS_STAT_ZMIN(ss, name, fs_stat)			\
do {								\
	if (!fs_stat->s.name || fs_stat->s.name > ss->name) {	\
		fs_stat->s.name = ss->name;			\
	}							\
} while (0)

#define _SCRUB_FS_STAT_ZMAX(ss, name, fs_stat)				\
do {									\
	if (!(fs_stat)->s.name || (fs_stat)->s.name < (ss)->name) {	\
		(fs_stat)->s.name = (ss)->name;				\
	}								\
} while (0)

static void add_to_fs_stat(struct btrfs_scrub_progress *p,
				struct scrub_stats *ss,
				struct scrub_fs_stat *fs_stat)
{
	_SCRUB_FS_STAT(p, data_extents_scrubbed, fs_stat);
	_SCRUB_FS_STAT(p, tree_extents_scrubbed, fs_stat);
	_SCRUB_FS_STAT(p, data_bytes_scrubbed, fs_stat);
	_SCRUB_FS_STAT(p, tree_bytes_scrubbed, fs_stat);
	_SCRUB_FS_STAT(p, read_errors, fs_stat);
	_SCRUB_FS_STAT(p, csum_errors, fs_stat);
	_SCRUB_FS_STAT(p, verify_errors, fs_stat);
	_SCRUB_FS_STAT(p, no_csum, fs_stat);
	_SCRUB_FS_STAT(p, csum_discards, fs_stat);
	_SCRUB_FS_STAT(p, super_errors, fs_stat);
	_SCRUB_FS_STAT(p, malloc_errors, fs_stat);
	_SCRUB_FS_STAT(p, uncorrectable_errors, fs_stat);
	_SCRUB_FS_STAT(p, corrected_errors, fs_stat);
	_SCRUB_FS_STAT(p, last_physical, fs_stat);
	_SCRUB_FS_STAT_ZMIN(ss, t_start, fs_stat);
	_SCRUB_FS_STAT_ZMIN(ss, t_resumed, fs_stat);
	_SCRUB_FS_STAT_ZMAX(ss, duration, fs_stat);
	_SCRUB_FS_STAT_ZMAX(ss, canceled, fs_stat);
	_SCRUB_FS_STAT_MIN(ss, finished, fs_stat);
}

static void init_fs_stat(struct scrub_fs_stat *fs_stat)
{
	memset(fs_stat, 0, sizeof(*fs_stat));
	fs_stat->s.finished = 1;
}

static void _print_scrub_ss(struct scrub_stats *ss)
{
	char t[4096];
	struct tm tm;

	if (!ss || !ss->t_start) {
		printf("\tno stats available\n");
		return;
	}
	if (ss->t_resumed) {
		localtime_r(&ss->t_resumed, &tm);
		strftime(t, sizeof(t), "%c", &tm);
		t[sizeof(t) - 1] = '\0';
		printf("\tscrub resumed at %s", t);
	} else {
		localtime_r(&ss->t_start, &tm);
		strftime(t, sizeof(t), "%c", &tm);
		t[sizeof(t) - 1] = '\0';
		printf("\tscrub started at %s", t);
	}
	if (ss->finished && !ss->canceled) {
		printf(" and finished after %llu seconds\n",
		       ss->duration);
	} else if (ss->canceled) {
		printf(" and was aborted after %llu seconds\n",
		       ss->duration);
	} else {
		printf(", running for %llu seconds\n", ss->duration);
	}
}

static void print_scrub_dev(struct btrfs_ioctl_dev_info_args *di,
				struct btrfs_scrub_progress *p, int raw,
				const char *append, struct scrub_stats *ss)
{
	printf("scrub device %s (id %llu) %s\n", di->path, di->devid,
	       append ? append : "");

	_print_scrub_ss(ss);

	if (p) {
		if (raw)
			print_scrub_full(p);
		else
			print_scrub_summary(p);
	}
}

static void print_fs_stat(struct scrub_fs_stat *fs_stat, int raw)
{
	_print_scrub_ss(&fs_stat->s);

	if (raw)
		print_scrub_full(&fs_stat->p);
	else
		print_scrub_summary(&fs_stat->p);
}

static void free_history(struct scrub_file_record **last_scrubs)
{
	struct scrub_file_record **l = last_scrubs;
	if (!l)
		return;
	while (*l)
		free(*l++);
	free(last_scrubs);
}

/*
 * cancels a running scrub and makes the master process record the current
 * progress status before exiting.
 */
static int cancel_fd = -1;
static void scrub_sigint_record_progress(int signal)
{
	int ret;

	ret = ioctl(cancel_fd, BTRFS_IOC_SCRUB_CANCEL, NULL);
	if (ret < 0)
		perror("Scrub cancel failed");
}

static int scrub_handle_sigint_parent(void)
{
	struct sigaction sa = {
		.sa_handler = SIG_IGN,
		.sa_flags = SA_RESTART,
	};

	return sigaction(SIGINT, &sa, NULL);
}

static int scrub_handle_sigint_child(int fd)
{
	struct sigaction sa = {
		.sa_handler = fd == -1 ? SIG_DFL : scrub_sigint_record_progress,
	};

	cancel_fd = fd;
	return sigaction(SIGINT, &sa, NULL);
}

static int scrub_datafile(const char *fn_base, const char *fn_local,
				const char *fn_tmp, char *datafile, int size)
{
	int ret;
	int end = size - 2;

	datafile[end + 1] = '\0';
	strncpy(datafile, fn_base, end);
	ret = strlen(datafile);

	if (ret + 1 > end)
		return -EOVERFLOW;

	datafile[ret] = '.';
	strncpy(datafile + ret + 1, fn_local, end - ret - 1);
	ret = strlen(datafile);

	if (ret + 1 > end)
		return -EOVERFLOW;

	if (fn_tmp) {
		datafile[ret] = '_';
		strncpy(datafile + ret + 1, fn_tmp, end - ret - 1);
		ret = strlen(datafile);

		if (ret > end)
			return -EOVERFLOW;
	}

	return 0;
}

static int scrub_open_file(const char *datafile, int m)
{
	int fd;
	int ret;

	fd = open(datafile, m, 0600);
	if (fd < 0)
		return -errno;

	ret = flock(fd, LOCK_EX|LOCK_NB);
	if (ret) {
		ret = errno;
		close(fd);
		return -ret;
	}

	return fd;
}

static int scrub_open_file_r(const char *fn_base, const char *fn_local)
{
	int ret;
	char datafile[BTRFS_PATH_NAME_MAX + 1];
	ret = scrub_datafile(fn_base, fn_local, NULL,
				datafile, sizeof(datafile));
	if (ret < 0)
		return ret;
	return scrub_open_file(datafile, O_RDONLY);
}

static int scrub_open_file_w(const char *fn_base, const char *fn_local,
				const char *tmp)
{
	int ret;
	char datafile[BTRFS_PATH_NAME_MAX + 1];
	ret = scrub_datafile(fn_base, fn_local, tmp,
				datafile, sizeof(datafile));
	if (ret < 0)
		return ret;
	return scrub_open_file(datafile, O_WRONLY|O_CREAT);
}

static int scrub_rename_file(const char *fn_base, const char *fn_local,
				const char *tmp)
{
	int ret;
	char datafile_old[BTRFS_PATH_NAME_MAX + 1];
	char datafile_new[BTRFS_PATH_NAME_MAX + 1];
	ret = scrub_datafile(fn_base, fn_local, tmp,
				datafile_old, sizeof(datafile_old));
	if (ret < 0)
		return ret;
	ret = scrub_datafile(fn_base, fn_local, NULL,
				datafile_new, sizeof(datafile_new));
	if (ret < 0)
		return ret;
	ret = rename(datafile_old, datafile_new);
	return ret ? -errno : 0;
}

#define _SCRUB_KVREAD(ret, i, name, avail, l, dest) if (ret == 0) {	  \
	ret = scrub_kvread(i, sizeof(#name), avail, l, #name, dest.name); \
}

/*
 * returns 0 if the key did not match (nothing was read)
 *         1 if the key did match (success)
 *        -1 if the key did match and an error occured
 */
static int scrub_kvread(int *i, int len, int avail, const char *buf,
			const char *key, u64 *dest)
{
	int j;

	if (*i + len + 1 < avail && strncmp(&buf[*i], key, len - 1) == 0) {
		*i += len - 1;
		if (buf[*i] != ':')
			return -1;
		*i += 1;
		for (j = 0; isdigit(buf[*i + j]) && *i + j < avail; ++j)
			;
		if (*i + j >= avail)
			return -1;
		*dest = atoll(&buf[*i]);
		*i += j;
		return 1;
	}

	return 0;
}

#define _SCRUB_INVALID do {						\
	if (report_errors)						\
		fprintf(stderr, "WARNING: invalid data in line %d pos "	\
			"%d state %d (near \"%.*s\") at %s:%d\n",	\
			lineno, i, state, 20 > avail ? avail : 20,	\
			l + i,	__FILE__, __LINE__);			\
	goto skip;							\
} while (0)

static struct scrub_file_record **scrub_read_file(int fd, int report_errors)
{
	int avail = 0;
	int old_avail = 0;
	char l[16 * 1024];
	int state = 0;
	int curr = -1;
	int i = 0;
	int j;
	int ret;
	int eof = 0;
	int lineno = 0;
	u64 version;
	char empty_uuid[BTRFS_FSID_SIZE] = {0};
	struct scrub_file_record **p = NULL;

	if (fd < 0)
		return ERR_PTR(-EINVAL);

again:
	old_avail = avail - i;
	BUG_ON(old_avail < 0);
	if (old_avail)
		memmove(l, l + i, old_avail);
	avail = read(fd, l + old_avail, sizeof(l) - old_avail);
	if (avail == 0)
		eof = 1;
	if (avail == 0 && old_avail == 0) {
		if (curr >= 0 &&
		    memcmp(p[curr]->fsid, empty_uuid, BTRFS_FSID_SIZE) == 0) {
			p[curr] = NULL;
		} else if (curr == -1) {
			p = ERR_PTR(-ENODATA);
		}
		return p;
	}
	if (avail == -1)
		return ERR_PTR(-errno);
	avail += old_avail;

	i = 0;
	while (i < avail) {
		switch (state) {
		case 0: /* start of file */
			ret = scrub_kvread(&i,
				sizeof(SCRUB_FILE_VERSION_PREFIX), avail, l,
				SCRUB_FILE_VERSION_PREFIX, &version);
			if (ret != 1)
				_SCRUB_INVALID;
			if (version != atoll(SCRUB_FILE_VERSION))
				return ERR_PTR(-ENOTSUP);
			state = 6;
			continue;
		case 1: /* start of line, alloc */
			/*
			 * this state makes sure we have a complete line in
			 * further processing, so we don't need wrap-tracking
			 * everywhere.
			 */
			if (!eof && !memchr(l + i, '\n', avail - i))
				goto again;
			++lineno;
			if (curr > -1 && memcmp(p[curr]->fsid, empty_uuid,
						BTRFS_FSID_SIZE) == 0) {
				state = 2;
				continue;
			}
			++curr;
			p = realloc(p, (curr + 2) * sizeof(*p));
			if (p)
				p[curr] = malloc(sizeof(**p));
			if (!p || !p[curr])
				return ERR_PTR(-errno);
			memset(p[curr], 0, sizeof(**p));
			p[curr + 1] = NULL;
			++state;
			/* fall through */
		case 2: /* start of line, skip space */
			while (isspace(l[i]) && i < avail) {
				if (l[i] == '\n')
					++lineno;
				++i;
			}
			if (i >= avail ||
			    (!eof && !memchr(l + i, '\n', avail - i)))
				goto again;
			++state;
			/* fall through */
		case 3: /* read fsid */
			if (i == avail)
				continue;
			for (j = 0; l[i + j] != ':' && i + j < avail; ++j)
				;
			if (i + j + 1 >= avail)
				_SCRUB_INVALID;
			if (j != 36)
				_SCRUB_INVALID;
			l[i + j] = '\0';
			ret = uuid_parse(l + i, p[curr]->fsid);
			if (ret)
				_SCRUB_INVALID;
			i += j + 1;
			++state;
			/* fall through */
		case 4: /* read dev id */
			for (j = 0; isdigit(l[i + j]) && i+j < avail; ++j)
				;
			if (j == 0 || i + j + 1 >= avail)
				_SCRUB_INVALID;
			p[curr]->devid = atoll(&l[i]);
			i += j + 1;
			++state;
			/* fall through */
		case 5: /* read key/value pair */
			ret = 0;
			_SCRUB_KVREAD(ret, &i, data_extents_scrubbed, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, data_extents_scrubbed, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, tree_extents_scrubbed, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, data_bytes_scrubbed, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, tree_bytes_scrubbed, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, read_errors, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, csum_errors, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, verify_errors, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, no_csum, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, csum_discards, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, super_errors, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, malloc_errors, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, uncorrectable_errors, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, corrected_errors, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, last_physical, avail, l,
					&p[curr]->p);
			_SCRUB_KVREAD(ret, &i, finished, avail, l,
					&p[curr]->stats);
			_SCRUB_KVREAD(ret, &i, t_start, avail, l,
					(u64 *)&p[curr]->stats);
			_SCRUB_KVREAD(ret, &i, t_resumed, avail, l,
					(u64 *)&p[curr]->stats);
			_SCRUB_KVREAD(ret, &i, duration, avail, l,
					(u64 *)&p[curr]->stats);
			_SCRUB_KVREAD(ret, &i, canceled, avail, l,
					&p[curr]->stats);
			if (ret != 1)
				_SCRUB_INVALID;
			++state;
			/* fall through */
		case 6: /* after number */
			if (l[i] == '|')
				state = 5;
			else if (l[i] == '\n')
				state = 1;
			else
				_SCRUB_INVALID;
			++i;
			continue;
		case 99: /* skip rest of line */
skip:
			state = 99;
			do {
				++i;
				if (l[i - 1] == '\n') {
					state = 1;
					break;
				}
			} while (i < avail);
			continue;
		}
		BUG();
	}
	goto again;
}

static int scrub_write_buf(int fd, const void *data, int len)
{
	int ret;
	ret = write(fd, data, len);
	return ret - len;
}

static int scrub_writev(int fd, char *buf, int max, const char *fmt, ...)
				__attribute__ ((format (printf, 4, 5)));
static int scrub_writev(int fd, char *buf, int max, const char *fmt, ...)
{
	int ret;
	va_list args;

	va_start(args, fmt);
	ret = vsnprintf(buf, max, fmt, args);
	va_end(args);
	if (ret >= max)
		return ret - max;
	return scrub_write_buf(fd, buf, ret);
}

#define _SCRUB_SUM(dest, data, name) dest->scrub_args.progress.name =	\
			data->resumed->p.name + data->scrub_args.progress.name

static struct scrub_progress *scrub_resumed_stats(struct scrub_progress *data,
						  struct scrub_progress *dest)
{
	if (!data->resumed || data->skip)
		return data;

	_SCRUB_SUM(dest, data, data_extents_scrubbed);
	_SCRUB_SUM(dest, data, tree_extents_scrubbed);
	_SCRUB_SUM(dest, data, data_bytes_scrubbed);
	_SCRUB_SUM(dest, data, tree_bytes_scrubbed);
	_SCRUB_SUM(dest, data, read_errors);
	_SCRUB_SUM(dest, data, csum_errors);
	_SCRUB_SUM(dest, data, verify_errors);
	_SCRUB_SUM(dest, data, no_csum);
	_SCRUB_SUM(dest, data, csum_discards);
	_SCRUB_SUM(dest, data, super_errors);
	_SCRUB_SUM(dest, data, malloc_errors);
	_SCRUB_SUM(dest, data, uncorrectable_errors);
	_SCRUB_SUM(dest, data, corrected_errors);
	_SCRUB_SUM(dest, data, last_physical);
	dest->stats.canceled = data->stats.canceled;
	dest->stats.finished = data->stats.finished;
	dest->stats.t_resumed = data->stats.t_start;
	dest->stats.t_start = data->resumed->stats.t_start;
	dest->stats.duration = data->resumed->stats.duration +
							data->stats.duration;
	dest->scrub_args.devid = data->scrub_args.devid;
	return dest;
}

#define _SCRUB_KVWRITE(fd, buf, name, use)		\
	scrub_kvwrite(fd, buf, sizeof(buf), #name,	\
			use->scrub_args.progress.name)

#define _SCRUB_KVWRITE_STATS(fd, buf, name, use)	\
	scrub_kvwrite(fd, buf, sizeof(buf), #name,	\
			use->stats.name)

static int scrub_kvwrite(int fd, char *buf, int max, const char *key, u64 val)
{
	return scrub_writev(fd, buf, max, "|%s:%lld", key, val);
}

static int scrub_write_file(int fd, const char *fsid,
				struct scrub_progress *data, int n)
{
	int ret = 0;
	int i;
	char buf[1024];
	struct scrub_progress local;
	struct scrub_progress *use;

	if (n < 1)
		return -EINVAL;

	/* each -1 is to subtract one \0 byte, the + 2 is for ':' and '\n' */
	ret = scrub_write_buf(fd, SCRUB_FILE_VERSION_PREFIX ":"
				SCRUB_FILE_VERSION "\n",
				(sizeof(SCRUB_FILE_VERSION_PREFIX) - 1) +
				(sizeof(SCRUB_FILE_VERSION) - 1) + 2);
	if (ret)
		return -EOVERFLOW;

	for (i = 0; i < n; ++i) {
		use = scrub_resumed_stats(&data[i], &local);
		if (scrub_write_buf(fd, fsid, strlen(fsid)) ||
		    scrub_write_buf(fd, ":", 1) ||
		    scrub_writev(fd, buf, sizeof(buf), "%lld",
					use->scrub_args.devid) ||
		    scrub_write_buf(fd, buf, ret) ||
		    _SCRUB_KVWRITE(fd, buf, data_extents_scrubbed, use) ||
		    _SCRUB_KVWRITE(fd, buf, tree_extents_scrubbed, use) ||
		    _SCRUB_KVWRITE(fd, buf, data_bytes_scrubbed, use) ||
		    _SCRUB_KVWRITE(fd, buf, tree_bytes_scrubbed, use) ||
		    _SCRUB_KVWRITE(fd, buf, read_errors, use) ||
		    _SCRUB_KVWRITE(fd, buf, csum_errors, use) ||
		    _SCRUB_KVWRITE(fd, buf, verify_errors, use) ||
		    _SCRUB_KVWRITE(fd, buf, no_csum, use) ||
		    _SCRUB_KVWRITE(fd, buf, csum_discards, use) ||
		    _SCRUB_KVWRITE(fd, buf, super_errors, use) ||
		    _SCRUB_KVWRITE(fd, buf, malloc_errors, use) ||
		    _SCRUB_KVWRITE(fd, buf, uncorrectable_errors, use) ||
		    _SCRUB_KVWRITE(fd, buf, corrected_errors, use) ||
		    _SCRUB_KVWRITE(fd, buf, last_physical, use) ||
		    _SCRUB_KVWRITE_STATS(fd, buf, t_start, use) ||
		    _SCRUB_KVWRITE_STATS(fd, buf, t_resumed, use) ||
		    _SCRUB_KVWRITE_STATS(fd, buf, duration, use) ||
		    _SCRUB_KVWRITE_STATS(fd, buf, canceled, use) ||
		    _SCRUB_KVWRITE_STATS(fd, buf, finished, use) ||
		    scrub_write_buf(fd, "\n", 1)) {
			return -EOVERFLOW;
		}
	}

	return 0;
}

static int scrub_write_progress(pthread_mutex_t *m, const char *fsid,
				struct scrub_progress *data, int n)
{
	int ret;
	int err;
	int fd = -1;
	int old;

	ret = pthread_mutex_lock(m);
	if (ret) {
		err = -ret;
		goto out;
	}

	ret = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
	if (ret) {
		err = -ret;
		goto out;
	}

	fd = scrub_open_file_w(SCRUB_DATA_FILE, fsid, "tmp");
	if (fd < 0) {
		err = fd;
		goto out;
	}
	err = scrub_write_file(fd, fsid, data, n);
	if (err)
		goto out;
	err = scrub_rename_file(SCRUB_DATA_FILE, fsid, "tmp");
	if (err)
		goto out;

out:
	if (fd >= 0) {
		ret = close(fd);
		if (ret)
			err = -errno;
	}

	ret = pthread_mutex_unlock(m);
	if (ret && !err)
		err = -ret;

	ret = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old);
	if (ret && !err)
		err = -ret;

	return err;
}

static void *scrub_one_dev(void *ctx)
{
	struct scrub_progress *sp = ctx;
	int ret;
	struct timeval tv;

	sp->stats.canceled = 0;
	sp->stats.duration = 0;
	sp->stats.finished = 0;

	ret = syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0,
		      IOPRIO_PRIO_VALUE(sp->ioprio_class,
					sp->ioprio_classdata));
	if (ret)
		fprintf(stderr,
			"WARNING: setting ioprio failed: %s (ignored).\n",
			strerror(errno));

	ret = ioctl(sp->fd, BTRFS_IOC_SCRUB, &sp->scrub_args);
	gettimeofday(&tv, NULL);
	sp->ret = ret;
	sp->stats.duration = tv.tv_sec - sp->stats.t_start;
	sp->stats.canceled = !!ret;
	sp->ioctl_errno = errno;
	ret = pthread_mutex_lock(&sp->progress_mutex);
	if (ret)
		return ERR_PTR(-ret);
	sp->stats.finished = 1;
	ret = pthread_mutex_unlock(&sp->progress_mutex);
	if (ret)
		return ERR_PTR(-ret);

	return NULL;
}

static void *progress_one_dev(void *ctx)
{
	struct scrub_progress *sp = ctx;

	sp->ret = ioctl(sp->fd, BTRFS_IOC_SCRUB_PROGRESS, &sp->scrub_args);
	sp->ioctl_errno = errno;

	return NULL;
}

/* nb: returns a negative errno via ERR_PTR */
static void *scrub_progress_cycle(void *ctx)
{
	int ret;
	int  perr = 0;	/* positive / pthread error returns */
	int old;
	int i;
	char fsid[37];
	struct scrub_progress *sp;
	struct scrub_progress *sp_last;
	struct scrub_progress *sp_shared;
	struct timeval tv;
	struct scrub_progress_cycle *spc = ctx;
	int ndev = spc->fi->num_devices;
	int this = 1;
	int last = 0;
	int peer_fd = -1;
	struct pollfd accept_poll_fd = {
		.fd = spc->prg_fd,
		.events = POLLIN,
		.revents = 0,
	};
	struct pollfd write_poll_fd = {
		.events = POLLOUT,
		.revents = 0,
	};
	struct sockaddr_un peer;
	socklen_t peer_size = sizeof(peer);

	perr = pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old);
	if (perr)
		goto out;

	uuid_unparse(spc->fi->fsid, fsid);

	for (i = 0; i < ndev; ++i) {
		sp = &spc->progress[i];
		sp_last = &spc->progress[i + ndev];
		sp_shared = &spc->shared_progress[i];
		sp->scrub_args.devid = sp_last->scrub_args.devid =
						sp_shared->scrub_args.devid;
		sp->fd = sp_last->fd = spc->fdmnt;
		sp->stats.t_start = sp_last->stats.t_start =
						sp_shared->stats.t_start;
		sp->resumed = sp_last->resumed = sp_shared->resumed;
		sp->skip = sp_last->skip = sp_shared->skip;
		sp->stats.finished = sp_last->stats.finished =
						sp_shared->stats.finished;
	}

	while (1) {
		ret = poll(&accept_poll_fd, 1, 5 * 1000);
		if (ret == -1) {
			ret = -errno;
			goto out;
		}
		if (ret)
			peer_fd = accept(spc->prg_fd, (struct sockaddr *)&peer,
					 &peer_size);
		gettimeofday(&tv, NULL);
		this = (this + 1)%2;
		last = (last + 1)%2;
		for (i = 0; i < ndev; ++i) {
			sp = &spc->progress[this * ndev + i];
			sp_last = &spc->progress[last * ndev + i];
			sp_shared = &spc->shared_progress[i];
			if (sp->stats.finished)
				continue;
			progress_one_dev(sp);
			sp->stats.duration = tv.tv_sec - sp->stats.t_start;
			if (!sp->ret)
				continue;
			if (sp->ioctl_errno != ENOTCONN &&
			    sp->ioctl_errno != ENODEV) {
				ret = -sp->ioctl_errno;
				goto out;
			}
			/*
			 * scrub finished or device removed, check the
			 * finished flag. if unset, just use the last
			 * result we got for the current write and go
			 * on. flag should be set on next cycle, then.
			 */
			perr = pthread_mutex_lock(&sp_shared->progress_mutex);
			if (perr)
				goto out;
			if (!sp_shared->stats.finished) {
				perr = pthread_mutex_unlock(
						&sp_shared->progress_mutex);
				if (perr)
					goto out;
				memcpy(sp, sp_last, sizeof(*sp));
				continue;
			}
			perr = pthread_mutex_unlock(&sp_shared->progress_mutex);
			if (perr)
				goto out;
			memcpy(sp, sp_shared, sizeof(*sp));
			memcpy(sp_last, sp_shared, sizeof(*sp));
		}
		if (peer_fd != -1) {
			write_poll_fd.fd = peer_fd;
			ret = poll(&write_poll_fd, 1, 0);
			if (ret == -1) {
				ret = -errno;
				goto out;
			}
			if (ret) {
				ret = scrub_write_file(
					peer_fd, fsid,
					&spc->progress[this * ndev], ndev);
				if (ret)
					goto out;
			}
			close(peer_fd);
			peer_fd = -1;
		}
		if (!spc->do_record)
			continue;
		ret = scrub_write_progress(spc->write_mutex, fsid,
					   &spc->progress[this * ndev], ndev);
		if (ret)
			goto out;
	}
out:
	if (peer_fd != -1)
		close(peer_fd);
	if (perr)
		ret = -perr;
	return ERR_PTR(ret);
}

static struct scrub_file_record *last_dev_scrub(
		struct scrub_file_record *const *const past_scrubs, u64 devid)
{
	int i;

	if (!past_scrubs || IS_ERR(past_scrubs))
		return NULL;

	for (i = 0; past_scrubs[i]; ++i)
		if (past_scrubs[i]->devid == devid)
			return past_scrubs[i];

	return NULL;
}

int mkdir_p(char *path)
{
	int i;
	int ret;

	for (i = 1; i < strlen(path); ++i) {
		if (path[i] != '/')
			continue;
		path[i] = '\0';
		ret = mkdir(path, 0777);
		if (ret && errno != EEXIST)
			return 1;
		path[i] = '/';
	}

	return 0;
}

static int is_scrub_running_on_fs(struct btrfs_ioctl_fs_info_args *fi_args,
				  struct btrfs_ioctl_dev_info_args *di_args,
				  struct scrub_file_record **past_scrubs)
{
	int i;

	if (!fi_args || !di_args || !past_scrubs)
		return 0;

	for (i = 0; i < fi_args->num_devices; i++) {
		struct scrub_file_record *sfr =
			last_dev_scrub(past_scrubs, di_args[i].devid);

		if (!sfr)
			continue;
		if (!(sfr->stats.finished || sfr->stats.canceled))
			return 1;
	}
	return 0;
}

static const char * const cmd_scrub_start_usage[];
static const char * const cmd_scrub_resume_usage[];

static int scrub_start(int argc, char **argv, int resume)
{
	int fdmnt;
	int prg_fd = -1;
	int fdres = -1;
	int ret;
	pid_t pid;
	int c;
	int i;
	int err = 0;
	int e_uncorrectable = 0;
	int e_correctable = 0;
	int print_raw = 0;
	char *path;
	int do_background = 1;
	int do_wait = 0;
	int do_print = 0;
	int do_quiet = 0;
	int do_record = 1;
	int readonly = 0;
	int do_stats_per_dev = 0;
	int ioprio_class = IOPRIO_CLASS_IDLE;
	int ioprio_classdata = 0;
	int n_start = 0;
	int n_skip = 0;
	int n_resume = 0;
	struct btrfs_ioctl_fs_info_args fi_args;
	struct btrfs_ioctl_dev_info_args *di_args = NULL;
	struct scrub_progress *sp = NULL;
	struct scrub_fs_stat fs_stat;
	struct timeval tv;
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
	};
	pthread_t *t_devs = NULL;
	pthread_t t_prog;
	pthread_attr_t t_attr;
	struct scrub_file_record **past_scrubs = NULL;
	struct scrub_file_record *last_scrub = NULL;
	char *datafile = strdup(SCRUB_DATA_FILE);
	char fsid[37];
	char sock_path[BTRFS_PATH_NAME_MAX + 1] = "";
	struct scrub_progress_cycle spc;
	pthread_mutex_t spc_write_mutex = PTHREAD_MUTEX_INITIALIZER;
	void *terr;
	u64 devid;

	optind = 1;
	while ((c = getopt(argc, argv, "BdqrRc:n:")) != -1) {
		switch (c) {
		case 'B':
			do_background = 0;
			do_wait = 1;
			do_print = 1;
			break;
		case 'd':
			do_stats_per_dev = 1;
			break;
		case 'q':
			do_quiet = 1;
			break;
		case 'r':
			readonly = 1;
			break;
		case 'R':
			print_raw = 1;
			break;
		case 'c':
			ioprio_class = (int)strtol(optarg, NULL, 10);
			break;
		case 'n':
			ioprio_classdata = (int)strtol(optarg, NULL, 10);
			break;
		case '?':
		default:
			usage(resume ? cmd_scrub_resume_usage :
						cmd_scrub_start_usage);
		}
	}

	/* try to catch most error cases before forking */

	if (check_argc_exact(argc - optind, 1)) {
		usage(resume ? cmd_scrub_resume_usage :
					cmd_scrub_start_usage);
	}

	spc.progress = NULL;
	if (do_quiet && do_print)
		do_print = 0;

	if (mkdir_p(datafile)) {
		ERR(!do_quiet, "WARNING: cannot create scrub data "
			       "file, mkdir %s failed: %s. Status recording "
			       "disabled\n", datafile, strerror(errno));
		do_record = 0;
	}
	free(datafile);

	path = argv[optind];

	fdmnt = open_path_or_dev_mnt(path);

	if (fdmnt < 0) {
		ERR(!do_quiet, "ERROR: can't access '%s'\n", path);
		return 12;
	}

	ret = get_fs_info(path, &fi_args, &di_args);
	if (ret) {
		ERR(!do_quiet, "ERROR: getting dev info for scrub failed: "
		    "%s\n", strerror(-ret));
		err = 1;
		goto out;
	}
	if (!fi_args.num_devices) {
		ERR(!do_quiet, "ERROR: no devices found\n");
		err = 1;
		goto out;
	}

	uuid_unparse(fi_args.fsid, fsid);
	fdres = scrub_open_file_r(SCRUB_DATA_FILE, fsid);
	if (fdres < 0 && fdres != -ENOENT) {
		ERR(!do_quiet, "WARNING: failed to open status file: "
		    "%s\n", strerror(-fdres));
	} else if (fdres >= 0) {
		past_scrubs = scrub_read_file(fdres, !do_quiet);
		if (IS_ERR(past_scrubs))
			ERR(!do_quiet, "WARNING: failed to read status file: "
			    "%s\n", strerror(-PTR_ERR(past_scrubs)));
		close(fdres);
	}

	/*
	 * check whether any involved device is already busy running a
	 * scrub. This would cause damaged status messages and the state
	 * "aborted" without the explanation that a scrub was already
	 * running. Therefore check it first, prevent it and give some
	 * feedback to the user if scrub is already running.
	 * Note that if scrub is started with a block device as the
	 * parameter, only that particular block device is checked. It
	 * is a normal mode of operation to start scrub on multiple
	 * single devices, there is no reason to prevent this.
	 */
	if (is_scrub_running_on_fs(&fi_args, di_args, past_scrubs)) {
		ERR(!do_quiet,
		    "ERROR: scrub is already running.\n"
		    "To cancel use 'btrfs scrub cancel %s'.\n"
		    "To see the status use 'btrfs scrub status [-d] %s'.\n",
		    path, path);
		err = 1;
		goto out;
	}

	t_devs = malloc(fi_args.num_devices * sizeof(*t_devs));
	sp = calloc(fi_args.num_devices, sizeof(*sp));
	spc.progress = calloc(fi_args.num_devices * 2, sizeof(*spc.progress));

	if (!t_devs || !sp || !spc.progress) {
		ERR(!do_quiet, "ERROR: scrub failed: %s", strerror(errno));
		err = 1;
		goto out;
	}

	ret = pthread_attr_init(&t_attr);
	if (ret) {
		ERR(!do_quiet, "ERROR: pthread_attr_init failed: %s\n",
		    strerror(ret));
		err = 1;
		goto out;
	}

	for (i = 0; i < fi_args.num_devices; ++i) {
		devid = di_args[i].devid;
		ret = pthread_mutex_init(&sp[i].progress_mutex, NULL);
		if (ret) {
			ERR(!do_quiet, "ERROR: pthread_mutex_init failed: "
			    "%s\n", strerror(ret));
			err = 1;
			goto out;
		}
		last_scrub = last_dev_scrub(past_scrubs, devid);
		sp[i].scrub_args.devid = devid;
		sp[i].fd = fdmnt;
		if (resume && last_scrub && (last_scrub->stats.canceled ||
					     !last_scrub->stats.finished)) {
			++n_resume;
			sp[i].scrub_args.start = last_scrub->p.last_physical;
			sp[i].resumed = last_scrub;
		} else if (resume) {
			++n_skip;
			sp[i].skip = 1;
			sp[i].resumed = last_scrub;
			continue;
		} else {
			++n_start;
			sp[i].scrub_args.start = 0ll;
			sp[i].resumed = NULL;
		}
		sp[i].skip = 0;
		sp[i].scrub_args.end = (u64)-1ll;
		sp[i].scrub_args.flags = readonly ? BTRFS_SCRUB_READONLY : 0;
		sp[i].ioprio_class = ioprio_class;
		sp[i].ioprio_classdata = ioprio_classdata;
	}

	if (!n_start && !n_resume) {
		if (!do_quiet)
			printf("scrub: nothing to resume for %s, fsid %s\n",
			       path, fsid);
		err = 0;
		goto out;
	}

	ret = prg_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	while (ret != -1) {
		ret = scrub_datafile(SCRUB_PROGRESS_SOCKET_PATH, fsid, NULL,
					sock_path, sizeof(sock_path));
		/* ignore EOVERFLOW, try using a shorter path for the socket */
		addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
		strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
		ret = bind(prg_fd, (struct sockaddr *)&addr, sizeof(addr));
		if (ret != -1 || errno != EADDRINUSE)
			break;
		/*
		 * bind failed with EADDRINUSE. so let's see if anyone answers
		 * when we make a call to the socket ...
		 */
		ret = connect(prg_fd, (struct sockaddr *)&addr, sizeof(addr));
		if (!ret || errno != ECONNREFUSED) {
			/* ... yes, so scrub must be running. error out */
			fprintf(stderr, "ERROR: scrub already running\n");
			close(prg_fd);
			prg_fd = -1;
			goto out;
		}
		/*
		 * ... no, this means someone left us alone with an unused
		 * socket in the file system. remove it and try again.
		 */
		ret = unlink(sock_path);
	}
	if (ret != -1)
		ret = listen(prg_fd, 100);
	if (ret == -1) {
		ERR(!do_quiet, "WARNING: failed to open the progress status "
		    "socket at %s: %s. Progress cannot be queried\n",
		    sock_path[0] ? sock_path : SCRUB_PROGRESS_SOCKET_PATH,
		    strerror(errno));
		if (prg_fd != -1) {
			close(prg_fd);
			prg_fd = -1;
			if (sock_path[0])
				unlink(sock_path);
		}
	}

	if (do_record) {
		/* write all-zero progress file for a start */
		ret = scrub_write_progress(&spc_write_mutex, fsid, sp,
					   fi_args.num_devices);
		if (ret) {
			ERR(!do_quiet, "WARNING: failed to write the progress "
			    "status file: %s. Status recording disabled\n",
			    strerror(-ret));
			do_record = 0;
		}
	}

	if (do_background) {
		pid = fork();
		if (pid == -1) {
			ERR(!do_quiet, "ERROR: cannot scrub, fork failed: "
					"%s\n", strerror(errno));
			err = 1;
			goto out;
		}

		if (pid) {
			int stat;
			scrub_handle_sigint_parent();
			if (!do_quiet)
				printf("scrub %s on %s, fsid %s (pid=%d)\n",
				       n_start ? "started" : "resumed",
				       path, fsid, pid);
			if (!do_wait) {
				err = 0;
				goto out;
			}
			ret = wait(&stat);
			if (ret != pid) {
				ERR(!do_quiet, "ERROR: wait failed: (ret=%d) "
				    "%s\n", ret, strerror(errno));
				err = 1;
				goto out;
			}
			if (!WIFEXITED(stat) || WEXITSTATUS(stat)) {
				ERR(!do_quiet, "ERROR: scrub process failed\n");
				err = WIFEXITED(stat) ? WEXITSTATUS(stat) : -1;
				goto out;
			}
			err = 0;
			goto out;
		}
	}

	scrub_handle_sigint_child(fdmnt);

	for (i = 0; i < fi_args.num_devices; ++i) {
		if (sp[i].skip) {
			sp[i].scrub_args.progress = sp[i].resumed->p;
			sp[i].stats = sp[i].resumed->stats;
			sp[i].ret = 0;
			sp[i].stats.finished = 1;
			continue;
		}
		devid = di_args[i].devid;
		gettimeofday(&tv, NULL);
		sp[i].stats.t_start = tv.tv_sec;
		ret = pthread_create(&t_devs[i], &t_attr,
					scrub_one_dev, &sp[i]);
		if (ret) {
			if (do_print)
				fprintf(stderr, "ERROR: creating "
					"scrub_one_dev[%llu] thread failed: "
					"%s\n", devid, strerror(ret));
			err = 1;
			goto out;
		}
	}

	spc.fdmnt = fdmnt;
	spc.prg_fd = prg_fd;
	spc.do_record = do_record;
	spc.write_mutex = &spc_write_mutex;
	spc.shared_progress = sp;
	spc.fi = &fi_args;
	ret = pthread_create(&t_prog, &t_attr, scrub_progress_cycle, &spc);
	if (ret) {
		if (do_print)
			fprintf(stderr, "ERROR: creating progress thread "
				"failed: %s\n", strerror(ret));
		err = 1;
		goto out;
	}

	err = 0;
	for (i = 0; i < fi_args.num_devices; ++i) {
		if (sp[i].skip)
			continue;
		devid = di_args[i].devid;
		ret = pthread_join(t_devs[i], NULL);
		if (ret) {
			if (do_print)
				fprintf(stderr, "ERROR: pthread_join failed "
					"for scrub_one_dev[%llu]: %s\n", devid,
					strerror(ret));
			++err;
			continue;
		}
		if (sp[i].ret && sp[i].ioctl_errno == ENODEV) {
			if (do_print)
				fprintf(stderr, "WARNING: device %lld not "
					"present\n", devid);
			continue;
		}
		if (sp[i].ret && sp[i].ioctl_errno == ECANCELED) {
			++err;
		} else if (sp[i].ret) {
			if (do_print)
				fprintf(stderr, "ERROR: scrubbing %s failed "
					"for device id %lld (%s)\n", path,
					devid, strerror(sp[i].ioctl_errno));
			++err;
			continue;
		}
		if (sp[i].scrub_args.progress.uncorrectable_errors > 0)
			e_uncorrectable++;
		if (sp[i].scrub_args.progress.corrected_errors > 0
		    || sp[i].scrub_args.progress.unverified_errors > 0)
			e_correctable++;
	}

	if (do_print) {
		const char *append = "done";
		if (!do_stats_per_dev)
			init_fs_stat(&fs_stat);
		for (i = 0; i < fi_args.num_devices; ++i) {
			if (do_stats_per_dev) {
				print_scrub_dev(&di_args[i],
						&sp[i].scrub_args.progress,
						print_raw,
						sp[i].ret ? "canceled" : "done",
						&sp[i].stats);
			} else {
				if (sp[i].ret)
					append = "canceled";
				add_to_fs_stat(&sp[i].scrub_args.progress,
						&sp[i].stats, &fs_stat);
			}
		}
		if (!do_stats_per_dev) {
			printf("scrub %s for %s\n", append, fsid);
			print_fs_stat(&fs_stat, print_raw);
		}
	}

	ret = pthread_cancel(t_prog);
	if (!ret)
		ret = pthread_join(t_prog, &terr);

	/* check for errors from the handling of the progress thread */
	if (do_print && ret) {
		fprintf(stderr, "ERROR: progress thread handling failed: %s\n",
			strerror(ret));
	}

	/* check for errors returned from the progress thread itself */
	if (do_print && terr && terr != PTHREAD_CANCELED) {
		fprintf(stderr, "ERROR: recording progress "
			"failed: %s\n", strerror(-PTR_ERR(terr)));
	}

	if (do_record) {
		ret = scrub_write_progress(&spc_write_mutex, fsid, sp,
					   fi_args.num_devices);
		if (ret && do_print) {
			fprintf(stderr, "ERROR: failed to record the result: "
				"%s\n", strerror(-ret));
		}
	}

	scrub_handle_sigint_child(-1);

out:
	free_history(past_scrubs);
	free(di_args);
	free(t_devs);
	free(sp);
	free(spc.progress);
	if (prg_fd > -1) {
		close(prg_fd);
		if (sock_path[0])
			unlink(sock_path);
	}
	close(fdmnt);

	if (err)
		return 1;
	if (e_correctable)
		return 7;
	if (e_uncorrectable)
		return 8;
	return 0;
}

static const char * const cmd_scrub_start_usage[] = {
	"btrfs scrub start [-Bdqr] [-c ioprio_class -n ioprio_classdata] <path>|<device>",
	"Start a new scrub",
	"",
	"-B     do not background",
	"-d     stats per device (-B only)",
	"-q     be quiet",
	"-r     read only mode",
	"-c     set ioprio class (see ionice(1) manpage)",
	"-n     set ioprio classdata (see ionice(1) manpage)",
	NULL
};

static int cmd_scrub_start(int argc, char **argv)
{
	return scrub_start(argc, argv, 0);
}

static const char * const cmd_scrub_cancel_usage[] = {
	"btrfs scrub cancel <path>|<device>",
	"Cancel a running scrub",
	NULL
};

static int cmd_scrub_cancel(int argc, char **argv)
{
	char *path;
	int ret;
	int fdmnt = -1;

	if (check_argc_exact(argc, 2))
		usage(cmd_scrub_cancel_usage);

	path = argv[1];

	fdmnt = open_path_or_dev_mnt(path);
	if (fdmnt < 0) {
		fprintf(stderr, "ERROR: could not open %s: %s\n",
			path, strerror(errno));
		ret = 1;
		goto out;
	}

	ret = ioctl(fdmnt, BTRFS_IOC_SCRUB_CANCEL, NULL);

	if (ret < 0) {
		fprintf(stderr, "ERROR: scrub cancel failed on %s: %s\n", path,
			errno == ENOTCONN ? "not running" : strerror(errno));
		ret = 1;
		goto out;
	}

	ret = 0;
	printf("scrub cancelled\n");

out:
	if (fdmnt != -1)
		close(fdmnt);
	return ret;
}

static const char * const cmd_scrub_resume_usage[] = {
	"btrfs scrub resume [-Bdqr] [-c ioprio_class -n ioprio_classdata] <path>|<device>",
	"Resume previously canceled or interrupted scrub",
	"",
	"-B     do not background",
	"-d     stats per device (-B only)",
	"-q     be quiet",
	"-r     read only mode",
	"-c     set ioprio class (see ionice(1) manpage)",
	"-n     set ioprio classdata (see ionice(1) manpage)",
	NULL
};

static int cmd_scrub_resume(int argc, char **argv)
{
	return scrub_start(argc, argv, 1);
}

static const char * const cmd_scrub_status_usage[] = {
	"btrfs scrub status [-dR] <path>|<device>",
	"Show status of running or finished scrub",
	"",
	"-d     stats per device",
	"-R     print raw stats",
	NULL
};

static int cmd_scrub_status(int argc, char **argv)
{
	char *path;
	struct btrfs_ioctl_fs_info_args fi_args;
	struct btrfs_ioctl_dev_info_args *di_args = NULL;
	struct scrub_file_record **past_scrubs = NULL;
	struct scrub_file_record *last_scrub;
	struct scrub_fs_stat fs_stat;
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
	};
	int ret;
	int i;
	int fdmnt;
	int print_raw = 0;
	int do_stats_per_dev = 0;
	int c;
	char fsid[37];
	int fdres = -1;
	int err = 0;

	optind = 1;
	while ((c = getopt(argc, argv, "dR")) != -1) {
		switch (c) {
		case 'd':
			do_stats_per_dev = 1;
			break;
		case 'R':
			print_raw = 1;
			break;
		case '?':
		default:
			usage(cmd_scrub_status_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_scrub_status_usage);

	path = argv[optind];

	fdmnt = open_path_or_dev_mnt(path);

	if (fdmnt < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 12;
	}

	ret = get_fs_info(path, &fi_args, &di_args);
	if (ret) {
		fprintf(stderr, "ERROR: getting dev info for scrub failed: "
				"%s\n", strerror(-ret));
		err = 1;
		goto out;
	}
	if (!fi_args.num_devices) {
		fprintf(stderr, "ERROR: no devices found\n");
		err = 1;
		goto out;
	}

	uuid_unparse(fi_args.fsid, fsid);

	fdres = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fdres == -1) {
		fprintf(stderr, "ERROR: failed to create socket to "
			"receive progress information: %s\n",
			strerror(errno));
		err = 1;
		goto out;
	}
	scrub_datafile(SCRUB_PROGRESS_SOCKET_PATH, fsid,
			NULL, addr.sun_path, sizeof(addr.sun_path));
	/* ignore EOVERFLOW, just use shorter name and hope for the best */
	addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
	ret = connect(fdres, (struct sockaddr *)&addr, sizeof(addr));
	if (ret == -1) {
		close(fdres);
		fdres = scrub_open_file_r(SCRUB_DATA_FILE, fsid);
		if (fdres < 0 && fdres != -ENOENT) {
			fprintf(stderr, "WARNING: failed to open status file: "
				"%s\n", strerror(-fdres));
			err = 1;
			goto out;
		}
	}

	if (fdres >= 0) {
		past_scrubs = scrub_read_file(fdres, 1);
		if (IS_ERR(past_scrubs))
			fprintf(stderr, "WARNING: failed to read status: %s\n",
				strerror(-PTR_ERR(past_scrubs)));
	}

	printf("scrub status for %s\n", fsid);

	if (do_stats_per_dev) {
		for (i = 0; i < fi_args.num_devices; ++i) {
			last_scrub = last_dev_scrub(past_scrubs,
							di_args[i].devid);
			if (!last_scrub) {
				print_scrub_dev(&di_args[i], NULL, print_raw,
						NULL, NULL);
				continue;
			}
			print_scrub_dev(&di_args[i], &last_scrub->p, print_raw,
					last_scrub->stats.finished ?
							"history" : "status",
					&last_scrub->stats);
		}
	} else {
		init_fs_stat(&fs_stat);
		for (i = 0; i < fi_args.num_devices; ++i) {
			last_scrub = last_dev_scrub(past_scrubs,
							di_args[i].devid);
			if (!last_scrub)
				continue;
			add_to_fs_stat(&last_scrub->p, &last_scrub->stats,
					&fs_stat);
		}
		print_fs_stat(&fs_stat, print_raw);
	}

out:
	free_history(past_scrubs);
	free(di_args);
	if (fdres > -1)
		close(fdres);

	return err;
}

const struct cmd_group scrub_cmd_group = {
	scrub_cmd_group_usage, NULL, {
		{ "start", cmd_scrub_start, cmd_scrub_start_usage, NULL, 0 },
		{ "cancel", cmd_scrub_cancel, cmd_scrub_cancel_usage, NULL, 0 },
		{ "resume", cmd_scrub_resume, cmd_scrub_resume_usage, NULL, 0 },
		{ "status", cmd_scrub_status, cmd_scrub_status_usage, NULL, 0 },
		{ 0, 0, 0, 0, 0 }
	}
};

int cmd_scrub(int argc, char **argv)
{
	return handle_command_group(&scrub_cmd_group, argc, argv);
}
