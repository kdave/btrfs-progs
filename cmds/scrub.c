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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>
#include <sys/time.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <limits.h>
#include <dirent.h>
#include <getopt.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <time.h>
#include <uuid/uuid.h>
#include "kernel-lib/sizes.h"
#include "kernel-shared/volumes.h"
#include "common/defs.h"
#include "common/compat.h"
#include "common/messages.h"
#include "common/utils.h"
#include "common/open-utils.h"
#include "common/units.h"
#include "common/device-utils.h"
#include "common/sysfs-utils.h"
#include "common/string-table.h"
#include "common/string-utils.h"
#include "common/help.h"
#include "cmds/commands.h"

static unsigned unit_mode = UNITS_DEFAULT;

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
	int in_progress;
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
	u64 old_limit;
	u64 limit;
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
	pr_verbose(LOG_DEFAULT, "\tdata_extents_scrubbed: %lld\n", sp->data_extents_scrubbed);
	pr_verbose(LOG_DEFAULT, "\ttree_extents_scrubbed: %lld\n", sp->tree_extents_scrubbed);
	pr_verbose(LOG_DEFAULT, "\tdata_bytes_scrubbed: %lld\n", sp->data_bytes_scrubbed);
	pr_verbose(LOG_DEFAULT, "\ttree_bytes_scrubbed: %lld\n", sp->tree_bytes_scrubbed);
	pr_verbose(LOG_DEFAULT, "\tread_errors: %lld\n", sp->read_errors);
	pr_verbose(LOG_DEFAULT, "\tcsum_errors: %lld\n", sp->csum_errors);
	pr_verbose(LOG_DEFAULT, "\tverify_errors: %lld\n", sp->verify_errors);
	pr_verbose(LOG_DEFAULT, "\tno_csum: %lld\n", sp->no_csum);
	pr_verbose(LOG_DEFAULT, "\tcsum_discards: %lld\n", sp->csum_discards);
	pr_verbose(LOG_DEFAULT, "\tsuper_errors: %lld\n", sp->super_errors);
	pr_verbose(LOG_DEFAULT, "\tmalloc_errors: %lld\n", sp->malloc_errors);
	pr_verbose(LOG_DEFAULT, "\tuncorrectable_errors: %lld\n", sp->uncorrectable_errors);
	pr_verbose(LOG_DEFAULT, "\tunverified_errors: %lld\n", sp->unverified_errors);
	pr_verbose(LOG_DEFAULT, "\tcorrected_errors: %lld\n", sp->corrected_errors);
	pr_verbose(LOG_DEFAULT, "\tlast_physical: %lld\n", sp->last_physical);
}

#define PRINT_SCRUB_ERROR(test, desc) do {	\
	if (test)				\
		pr_verbose(LOG_DEFAULT, " %s=%llu", desc, test);	\
} while (0)

static void print_scrub_summary(struct btrfs_scrub_progress *p, struct scrub_stats *s,
				u64 bytes_total, u64 limit)
{
	u64 err_cnt;
	u64 err_cnt2;
	u64 bytes_scrubbed;
	u64 bytes_per_sec = 0;
	u64 sec_left = 0;
	time_t sec_eta;

	bytes_scrubbed = p->data_bytes_scrubbed + p->tree_bytes_scrubbed;
	/*
	 * If duration is zero seconds (rounded down), then the Rate metric
	 * should still reflect the amount of bytes that have been processed
	 * in under a second.
	 */
	if (s->duration == 0)
		bytes_per_sec = bytes_scrubbed;
	else
		bytes_per_sec = bytes_scrubbed / s->duration;
	if (bytes_per_sec > 0)
		sec_left = (bytes_total - bytes_scrubbed) / bytes_per_sec;

	err_cnt = p->read_errors +
			p->csum_errors +
			p->verify_errors +
			p->super_errors;

	err_cnt2 = p->corrected_errors + p->uncorrectable_errors;

	if (p->malloc_errors)
		pr_verbose(LOG_DEFAULT, "*** WARNING: memory allocation failed while scrubbing. "
		       "results may be inaccurate\n");

	if (s->in_progress) {
		char t[4096];
		struct tm tm;

		sec_eta = time(NULL);
		sec_eta += sec_left;
		localtime_r(&sec_eta, &tm);
		t[sizeof(t) - 1] = '\0';
		strftime(t, sizeof(t), "%c", &tm);

		pr_verbose(LOG_DEFAULT, "Time left:        %llu:%02llu:%02llu\n",
			sec_left / 3600, (sec_left / 60) % 60, sec_left % 60);
		pr_verbose(LOG_DEFAULT, "ETA:              %s\n", t);
		pr_verbose(LOG_DEFAULT, "Total to scrub:   %s\n",
			pretty_size_mode(bytes_total, unit_mode));
		pr_verbose(LOG_DEFAULT, "Bytes scrubbed:   %s  (%.2f%%)\n",
			pretty_size_mode(bytes_scrubbed, unit_mode),
			100.0 * bytes_scrubbed / bytes_total);
	} else {
		pr_verbose(LOG_DEFAULT, "Total to scrub:   %s\n",
			pretty_size_mode(bytes_total, unit_mode));
	}

	/*
	 * Rate and size units are disproportionate so they are affected only
	 * by --raw, otherwise it's human readable (respecting the SI or IEC mode).
	 */
	if (unit_mode == UNITS_RAW) {
		pr_verbose(LOG_DEFAULT, "Rate:             %s/s",
			pretty_size_mode(bytes_per_sec, UNITS_RAW));
		if (limit > 1)
			pr_verbose(LOG_DEFAULT, " (limit %s/s)",
				   pretty_size_mode(limit, UNITS_RAW));
		else if (limit == 1)
			pr_verbose(LOG_DEFAULT, " (some device limits set)");
		pr_verbose(LOG_DEFAULT, "\n");
	} else {
		unsigned int mode = UNITS_HUMAN_DECIMAL;

		if (unit_mode & UNITS_BINARY)
			mode = UNITS_HUMAN_BINARY;

		pr_verbose(LOG_DEFAULT, "Rate:             %s/s",
			pretty_size_mode(bytes_per_sec, mode));
		if (limit > 1)
			pr_verbose(LOG_DEFAULT, " (limit %s/s)",
				   pretty_size_mode(limit, mode));
		else if (limit == 1)
			pr_verbose(LOG_DEFAULT, " (some device limits set)");
		pr_verbose(LOG_DEFAULT, "\n");
	}

	pr_verbose(LOG_DEFAULT, "Error summary:   ");
	if (err_cnt || err_cnt2) {
		PRINT_SCRUB_ERROR(p->read_errors, "read");
		PRINT_SCRUB_ERROR(p->super_errors, "super");
		PRINT_SCRUB_ERROR(p->verify_errors, "verify");
		PRINT_SCRUB_ERROR(p->csum_errors, "csum");
		pr_verbose(LOG_DEFAULT, "\n");
		pr_verbose(LOG_DEFAULT, "  Corrected:      %llu\n", p->corrected_errors);
		pr_verbose(LOG_DEFAULT, "  Uncorrectable:  %llu\n", p->uncorrectable_errors);
		pr_verbose(LOG_DEFAULT, "  Unverified:     %llu\n", p->unverified_errors);
	} else {
		pr_verbose(LOG_DEFAULT, " no errors found\n");
	}
}

#define _SCRUB_FS_STAT(p, name, fs_stat) do {	\
	fs_stat->p.name += p->name;		\
} while (0)

#define _SCRUB_FS_STAT_COPY(p, name, fs_stat) do {	\
	fs_stat->p.name = p->name;		\
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
	_SCRUB_FS_STAT_COPY(p, last_physical, fs_stat);
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
	time_t seconds;
	unsigned hours;

	if (!ss || !ss->t_start) {
		pr_verbose(LOG_DEFAULT, "\tno stats available\n");
		return;
	}
	if (ss->t_resumed) {
		localtime_r(&ss->t_resumed, &tm);
		strftime(t, sizeof(t), "%c", &tm);
		t[sizeof(t) - 1] = '\0';
		pr_verbose(LOG_DEFAULT, "Scrub resumed:    %s\n", t);
	} else {
		localtime_r(&ss->t_start, &tm);
		strftime(t, sizeof(t), "%c", &tm);
		t[sizeof(t) - 1] = '\0';
		pr_verbose(LOG_DEFAULT, "Scrub started:    %s\n", t);
	}

	seconds = ss->duration;
	hours = ss->duration / (60 * 60);
	gmtime_r(&seconds, &tm);
	strftime(t, sizeof(t), "%M:%S", &tm);
	pr_verbose(LOG_DEFAULT, "Status:           %s\n",
			(ss->in_progress ? "running" :
			 (ss->canceled ? "aborted" :
			  (ss->finished ? "finished" : "interrupted"))));
	pr_verbose(LOG_DEFAULT, "Duration:         %u:%s\n", hours, t);
}

static void print_scrub_dev(struct btrfs_ioctl_dev_info_args *di,
				struct btrfs_scrub_progress *p, int raw,
				const char *append, struct scrub_stats *ss,
				u64 limit)
{
	pr_verbose(LOG_DEFAULT, "\nScrub device %s (id %llu) %s\n", di->path, di->devid,
	       append ? append : "");

	_print_scrub_ss(ss);

	if (p) {
		if (raw) {
			print_scrub_full(p);
		} else if (ss->finished) {
			/*
			 * For finished scrub, we can use the total scrubbed
			 * bytes to report "Total to scrub", which is more
			 * accurate (e.g. mostly empty block groups).
			 */
			print_scrub_summary(p, ss, p->data_bytes_scrubbed +
					    p->tree_bytes_scrubbed, limit);
		} else {
			/*
			 * For any canceled/interrupted/running scrub, we're
			 * not sure how many bytes we're really going to scrub,
			 * thus we use device's used bytes instead.
			 */
			print_scrub_summary(p, ss, di->bytes_used, limit);
		}
	}
}

/*
 * Print summary stats for the whole filesystem. If there's only one device
 * print the limit if set, otherwise a special value to print a note that
 * limits are set.
 */
static void print_fs_stat(struct scrub_fs_stat *fs_stat, int raw, u64 bytes_total,
			  u64 nr_devices, u64 limit)
{
	_print_scrub_ss(&fs_stat->s);

	if (raw) {
		print_scrub_full(&fs_stat->p);
	} else {
		/*
		 * Limit for the whole filesystem stats does not make sense,
		 * but if there's any device with a limit then print it.
		 */
		if (nr_devices != 1 && limit)
			limit = 1;
		print_scrub_summary(&fs_stat->p, &fs_stat->s, bytes_total, limit);
	}
}

static void free_history(struct scrub_file_record **last_scrubs)
{
	struct scrub_file_record **l = last_scrubs;
	if (!l || IS_ERR(l))
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
	char datafile[PATH_MAX];
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
	char datafile[PATH_MAX];
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
	char datafile_old[PATH_MAX];
	char datafile_new[PATH_MAX];
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
 *        -1 if the key did match and an error occurred
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
		warning("invalid data on line %d pos "			\
			"%d state %d (near \"%.*s\") at %s:%d",		\
			lineno, i, state, 20 > avail ? avail : 20,	\
			l + i,	__FILE__, __LINE__);			\
	goto skip;							\
} while (0)

static struct scrub_file_record **scrub_read_file(int fd, int report_errors)
{
	int avail = 0;
	int old_avail = 0;
	char l[SZ_16K];
	int state = 0;
	int curr = -1;
	int i = 0;
	int j;
	int ret;
	bool eof = false;
	int lineno = 0;
	u64 version;
	char empty_uuid[BTRFS_FSID_SIZE] = {0};
	struct scrub_file_record **p = NULL;

again:
	old_avail = avail - i;
	if (old_avail < 0) {
		error("scrub record file corrupted near byte %d", i);
		return ERR_PTR(-EINVAL);
	}
	if (old_avail)
		memmove(l, l + i, old_avail);
	avail = read(fd, l + old_avail, sizeof(l) - old_avail);
	if (avail == 0)
		eof = true;
	if (avail == 0 && old_avail == 0) {
		if (curr >= 0 &&
		    memcmp(p[curr]->fsid, empty_uuid, BTRFS_FSID_SIZE) == 0) {
			p[curr] = NULL;
		} else if (curr == -1) {
			p = ERR_PTR(-ENODATA);
		}
		return p;
	}
	if (avail == -1) {
		free_history(p);
		return ERR_PTR(-errno);
	}
	avail += old_avail;

	i = 0;
	while (i < avail) {
		void *tmp;

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
			tmp = p;
			p = realloc(p, (curr + 2) * sizeof(*p));
			if (!p) {
				free_history(tmp);
				return ERR_PTR(-errno);
			}
			p[curr] = malloc(sizeof(**p));
			if (!p[curr]) {
				free_history(p);
				return ERR_PTR(-errno);
			}
			memset(p[curr], 0, sizeof(**p));
			p[curr + 1] = NULL;
			++state;
			fallthrough;
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
			fallthrough;
		case 3: /* read fsid */
			if (i == avail)
				continue;
			for (j = 0; l[i + j] != ':' && i + j < avail; ++j)
				;
			if (i + j + 1 >= avail)
				_SCRUB_INVALID;
			if (j != BTRFS_UUID_UNPARSED_SIZE - 1)
				_SCRUB_INVALID;
			l[i + j] = '\0';
			ret = uuid_parse(l + i, p[curr]->fsid);
			if (ret)
				_SCRUB_INVALID;
			i += j + 1;
			++state;
			fallthrough;
		case 4: /* read dev id */
			for (j = 0; isdigit(l[i + j]) && i+j < avail; ++j)
				;
			if (j == 0 || i + j + 1 >= avail)
				_SCRUB_INVALID;
			p[curr]->devid = atoll(&l[i]);
			i += j + 1;
			++state;
			fallthrough;
		case 5: /* read key/value pair */
			ret = 0;
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
			fallthrough;
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
		error("internal error: unknown parser state %d near byte %d",
				state, i);
		return ERR_PTR(-EINVAL);
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
#define _SCRUB_COPY(dest, data, name) dest->scrub_args.progress.name =	\
			data->scrub_args.progress.name

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
	_SCRUB_COPY(dest, data, last_physical);
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

	ret = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
	if (ret) {
		err = -ret;
		goto out3;
	}

	ret = pthread_mutex_lock(m);
	if (ret) {
		err = -ret;
		goto out2;
	}

	fd = scrub_open_file_w(SCRUB_DATA_FILE, fsid, "tmp");
	if (fd < 0) {
		err = fd;
		goto out1;
	}
	err = scrub_write_file(fd, fsid, data, n);
	if (err)
		goto out1;
	err = scrub_rename_file(SCRUB_DATA_FILE, fsid, "tmp");
	if (err)
		goto out1;

out1:
	if (fd >= 0) {
		ret = close(fd);
		if (ret)
			err = -errno;
	}

	ret = pthread_mutex_unlock(m);
	if (ret && !err)
		err = -ret;

out2:
	ret = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old);
	if (ret && !err)
		err = -ret;

out3:
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
		warning("setting ioprio failed: %m (ignored)");

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
	int ret = 0;
	int  perr = 0;	/* positive / pthread error returns */
	int old;
	int i;
	char fsid[BTRFS_UUID_UNPARSED_SIZE];
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
			perr = pthread_setcancelstate(
					PTHREAD_CANCEL_DISABLE, &old);
			if (perr)
				goto out;
			perr = pthread_mutex_lock(&sp_shared->progress_mutex);
			if (perr)
				goto out;
			if (!sp_shared->stats.finished) {
				perr = pthread_mutex_unlock(
						&sp_shared->progress_mutex);
				if (perr)
					goto out;
				perr = pthread_setcancelstate(
						PTHREAD_CANCEL_ENABLE, &old);
				if (perr)
					goto out;
				memcpy(sp, sp_last, sizeof(*sp));
				continue;
			}
			perr = pthread_mutex_unlock(&sp_shared->progress_mutex);
			if (perr)
				goto out;
			perr = pthread_setcancelstate(
					PTHREAD_CANCEL_ENABLE, &old);
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

static int mkdir_p(char *path)
{
	int i;
	int ret;

	for (i = 1; i < strlen(path); ++i) {
		if (path[i] != '/')
			continue;
		path[i] = '\0';
		ret = mkdir(path, 0777);
		if (ret && errno != EEXIST)
			return -errno;
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

static int is_scrub_running_in_kernel(int fd,
		struct btrfs_ioctl_dev_info_args *di_args, u64 max_devices)
{
	struct scrub_progress sp;
	int i;
	int ret;

	for (i = 0; i < max_devices; i++) {
		memset(&sp, 0, sizeof(sp));
		sp.scrub_args.devid = di_args[i].devid;
		ret = ioctl(fd, BTRFS_IOC_SCRUB_PROGRESS, &sp.scrub_args);
		if (!ret)
			return 1;
	}

	return 0;
}

static u64 read_scrub_device_limit(int fd, u64 devid)
{
	char path[PATH_MAX] = { 0 };
	u64 limit;
	int ret;

	/* /sys/fs/btrfs/FSID/devinfo/1/scrub_speed_max */
	snprintf(path, sizeof(path), "devinfo/%llu/scrub_speed_max", devid);
	ret = sysfs_read_fsid_file_u64(fd, path, &limit);
	if (ret < 0)
		limit = 0;
	return limit;
}

static u64 write_scrub_device_limit(int fd, u64 devid, u64 limit)
{
	char path[PATH_MAX] = { 0 };
	int ret;

	/* /sys/fs/btrfs/FSID/devinfo/1/scrub_speed_max */
	snprintf(path, sizeof(path), "devinfo/%llu/scrub_speed_max", devid);
	ret = sysfs_write_fsid_file_u64(fd, path, limit);
	return ret;
}

static int scrub_start(const struct cmd_struct *cmd, int argc, char **argv,
		       bool resume)
{
	int fdmnt;
	int prg_fd = -1;
	int fdres = -1;
	int ret;
	pid_t pid;
	int i;
	int err = 0;
	int e_uncorrectable = 0;
	int e_correctable = 0;
	bool print_raw = false;
	char *path;
	bool do_background = true;
	bool do_wait = false;
	bool do_print = false;
	bool do_record = true;
	bool readonly = false;
	bool do_stats_per_dev = false;
	int ioprio_class = IOPRIO_CLASS_IDLE;
	int ioprio_classdata = 0;
	int n_start = 0;
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
	struct scrub_file_record **past_scrubs = NULL;
	struct scrub_file_record *last_scrub = NULL;
	char datafile[] = SCRUB_DATA_FILE;
	char fsid[BTRFS_UUID_UNPARSED_SIZE];
	char sock_path[PATH_MAX] = "";
	struct scrub_progress_cycle spc;
	pthread_mutex_t spc_write_mutex = PTHREAD_MUTEX_INITIALIZER;
	void *terr;
	u64 throughput_limit = 0;
	u64 devid;
	bool force = false;
	bool nothing_to_resume = false;

	while (1) {
		int c;
		enum { GETOPT_VAL_LIMIT = GETOPT_VAL_FIRST };
		static const struct option long_options[] = {
			{"limit", required_argument, NULL, GETOPT_VAL_LIMIT},
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "BdqrRc:n:f", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'B':
			do_background = false;
			do_wait = true;
			do_print = true;
			break;
		case 'd':
			do_stats_per_dev = true;
			break;
		case 'q':
			bconf_be_quiet();
			break;
		case 'r':
			readonly = true;
			break;
		case 'R':
			print_raw = true;
			break;
		case 'c':
			ioprio_class = (int)strtol(optarg, NULL, 10);
			break;
		case 'n':
			ioprio_classdata = (int)strtol(optarg, NULL, 10);
			break;
		case 'f':
			force = true;
			break;
		case GETOPT_VAL_LIMIT:
			throughput_limit = arg_strtou64_with_suffix(optarg);
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	/* try to catch most error cases before forking */

	if (check_argc_exact(argc - optind, 1))
		return 1;

	spc.progress = NULL;
	if (bconf.verbose == BTRFS_BCONF_QUIET && do_print)
		do_print = false;

	if (mkdir_p(datafile)) {
		warning("cannot create scrub data file, mkdir %s failed: %m, status recording disabled",
			datafile);
		do_record = false;
	}

	path = argv[optind];

	fdmnt = btrfs_open_mnt(path);
	if (fdmnt < 0)
		return 1;

	ret = get_fs_info(path, &fi_args, &di_args);
	if (ret) {
		errno = -ret;
		error("getting dev info for scrub failed: %m");
		err = 1;
		goto out;
	}
	if (!fi_args.num_devices) {
		error("no devices found");
		err = 1;
		goto out;
	}

	uuid_unparse(fi_args.fsid, fsid);
	fdres = scrub_open_file_r(SCRUB_DATA_FILE, fsid);
	if (fdres < 0 && fdres != -ENOENT) {
		errno = -fdres;
		warning("failed to open status file: %m");
	} else if (fdres >= 0) {
		past_scrubs = scrub_read_file(fdres, 1);
		if (IS_ERR(past_scrubs)) {
			errno = -PTR_ERR(past_scrubs);
			warning("failed to read status file: %m");
		}
		close(fdres);
	}

	/*
	 * Check for stale information in the status file, ie. if it's
	 * canceled=0, finished=0 but no scrub is running.
	 */
	if (!is_scrub_running_in_kernel(fdmnt, di_args, fi_args.num_devices))
		force = true;

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
	if (!force && is_scrub_running_on_fs(&fi_args, di_args, past_scrubs)) {
		error(  "Scrub is already running.\n"
			"To cancel use 'btrfs scrub cancel %s'.\n"
			"To see the status use 'btrfs scrub status [-d] %s'",
			path, path);
		err = 1;
		goto out;
	}

	t_devs = malloc(fi_args.num_devices * sizeof(*t_devs));
	sp = calloc(fi_args.num_devices, sizeof(*sp));
	spc.progress = calloc(fi_args.num_devices * 2, sizeof(*spc.progress));

	if (!t_devs || !sp || !spc.progress) {
		error("scrub failed: %m");
		err = 1;
		goto out;
	}

	for (i = 0; i < fi_args.num_devices; ++i) {
		devid = di_args[i].devid;
		sp[i].old_limit = read_scrub_device_limit(fdmnt, devid);
		ret = write_scrub_device_limit(fdmnt, devid, throughput_limit);
		if (ret < 0) {
			errno = -ret;
			warning("failed to set scrub throughput limit on devid %llu: %m",
				devid);
		}
		ret = pthread_mutex_init(&sp[i].progress_mutex, NULL);
		if (ret) {
			errno = ret;
			error("pthread_mutex_init failed: %m");
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
		sp[i].limit = read_scrub_device_limit(fdmnt, devid);
	}

	if (!n_start && !n_resume) {
		pr_verbose(LOG_DEFAULT,
			   "scrub: nothing to resume for %s, fsid %s\n",
			   path, fsid);
		nothing_to_resume = true;
		goto out;
	}

	ret = prg_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	while (ret != -1) {
		ret = scrub_datafile(SCRUB_PROGRESS_SOCKET_PATH, fsid, NULL,
					sock_path, sizeof(sock_path));
		/* ignore EOVERFLOW, try using a shorter path for the socket */
		addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
		strncpy_null(addr.sun_path, sock_path, sizeof(addr.sun_path));
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
			error("scrub already running");
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
		warning("failed to open the progress status socket at %s: %m, progress cannot be queried",
			sock_path[0] ? sock_path :
			SCRUB_PROGRESS_SOCKET_PATH);
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
			errno = -ret;
			warning("failed to write the progress status file: %m, status recording disabled");
			do_record = false;
		}
	}

	if (do_background) {
		pid = fork();
		if (pid == -1) {
			error("cannot scrub, fork failed: %m");
			err = 1;
			goto out;
		}

		if (pid) {
			int stat;
			scrub_handle_sigint_parent();
			pr_verbose(LOG_DEFAULT,
				   "scrub %s on %s, fsid %s (pid=%d)\n",
				   n_start ? "started" : "resumed",
				   path, fsid, pid);
			if (!do_wait) {
				err = 0;
				goto out;
			}
			ret = wait(&stat);
			if (ret != pid) {
				error("wait failed (ret=%d): %m", ret);
				err = 1;
				goto out;
			}
			if (!WIFEXITED(stat) || WEXITSTATUS(stat)) {
				err = WIFEXITED(stat) ? WEXITSTATUS(stat) : -1;
				error("scrub process failed with error %d", err);
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
		pr_verbose(LOG_DEFAULT, "Starting scrub on devid %llu", devid);
		if (sp[i].limit > 0)
			pr_verbose(LOG_DEFAULT, " (limit %s/s)\n", pretty_size(sp[i].limit));
		else
			pr_verbose(LOG_DEFAULT, "\n");

		ret = pthread_create(&t_devs[i], NULL,
					scrub_one_dev, &sp[i]);
		if (ret) {
			if (do_print) {
				errno = ret;
				error(
				"creating scrub_one_dev[%llu] thread failed: %m",
					devid);
			}
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
	ret = pthread_create(&t_prog, NULL, scrub_progress_cycle, &spc);
	if (ret) {
		if (do_print) {
			errno = ret;
			error("creating progress thread failed: %m");
		}
		err = 1;
		goto out;
	}

	err = 0;
	for (i = 0; i < fi_args.num_devices; ++i) {
		/* Revert to the older scrub limit. */
		ret = write_scrub_device_limit(fdmnt, di_args[i].devid, sp[i].old_limit);
		if (ret < 0) {
			errno = -ret;
			warning("failed to reset scrub throughput limit on devid %llu: %m",
				di_args[i].devid);
		}

		if (sp[i].skip)
			continue;
		devid = di_args[i].devid;
		ret = pthread_join(t_devs[i], NULL);
		if (ret) {
			if (do_print) {
				errno = ret;
				error(
				"pthread_join failed for scrub_one_dev[%llu]: %m",
					devid);
			}
			++err;
			continue;
		}
		if (sp[i].ret) {
			switch (sp[i].ioctl_errno) {
			case ENODEV:
				if (do_print)
					warning("device %lld not present",
						devid);
				continue;
			case ECANCELED:
				++err;
				break;
			default:
				if (do_print) {
					errno = sp[i].ioctl_errno;
					error(
		"scrubbing %s failed for device id %lld: ret=%d, errno=%d (%m)",
						path, devid, sp[i].ret,
						sp[i].ioctl_errno);
				}
				++err;
				continue;
			}
		}
		if (sp[i].scrub_args.progress.uncorrectable_errors > 0)
			e_uncorrectable++;
		if (sp[i].scrub_args.progress.corrected_errors > 0
		    || sp[i].scrub_args.progress.unverified_errors > 0)
			e_correctable++;
	}

	if (do_print) {
		const char *append = "done";
		u64 total_bytes_scrubbed = 0;
		u64 limit = 0;

		if (!do_stats_per_dev)
			init_fs_stat(&fs_stat);
		for (i = 0; i < fi_args.num_devices; ++i) {
			struct btrfs_scrub_progress *cur_progress =
						&sp[i].scrub_args.progress;

			/* On a multi-device filesystem, keep the lowest limit only. */
			if (!limit || (sp[i].limit && sp[i].limit < limit))
				limit = sp[i].limit;

			if (do_stats_per_dev) {
				print_scrub_dev(&di_args[i],
						cur_progress,
						print_raw,
						sp[i].ret ? "canceled" : "done",
						&sp[i].stats,
						sp[i].limit);
			} else {
				if (sp[i].ret)
					append = "canceled";
				add_to_fs_stat(cur_progress, &sp[i].stats, &fs_stat);
			}
			total_bytes_scrubbed += cur_progress->data_bytes_scrubbed +
						cur_progress->tree_bytes_scrubbed;
		}
		if (!do_stats_per_dev) {
			pr_verbose(LOG_DEFAULT, "scrub %s for %s\n", append, fsid);
			print_fs_stat(&fs_stat, print_raw, total_bytes_scrubbed,
				      fi_args.num_devices, limit);
		}
	}

	ret = pthread_cancel(t_prog);
	if (!ret)
		ret = pthread_join(t_prog, &terr);

	/* check for errors from the handling of the progress thread */
	if (do_print && ret) {
		errno = ret;
		error("progress thread handling failed: %m");
	}

	/* check for errors returned from the progress thread itself */
	if (do_print && terr && terr != PTHREAD_CANCELED) {
		errno = -PTR_ERR(terr);
		error("recording progress failed: %m");
	}

	if (do_record) {
		ret = scrub_write_progress(&spc_write_mutex, fsid, sp,
					   fi_args.num_devices);
		if (ret && do_print) {
			errno = -ret;
			error("failed to record the result: %m");
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
	if (nothing_to_resume)
		return 2;
	if (e_uncorrectable) {
		error("there are %d uncorrectable errors", e_uncorrectable);
		return 3;
	}
	if (e_correctable)
		warning("errors detected during scrubbing, %d corrected", e_correctable);

	return 0;
}

static const char * const cmd_scrub_start_usage[] = {
	"btrfs scrub start [options] <path>|<device>",
	"Start a new scrub on the filesystem or a device (can be started only once)",
	"",
	OPTLINE("-B", "do not background"),
	OPTLINE("-d", "stats per device (-B only)"),
	OPTLINE("-r", "read only mode (no repairs done)"),
	OPTLINE("-R", "raw print mode, print full data instead of summary"),
	OPTLINE("--limit SIZE", "set the throughput limit for each device (0 for unlimited), restored afterwards"),
	OPTLINE("-f", "force starting new scrub even if a scrub is already running this is useful when scrub stats record file is damaged"),
	OPTLINE("-q", "deprecated, alias for global -q option"),
	"",
	"Priority (requires IO scheduler support, not supported by mq-deadline):",
	OPTLINE("-c CLASS ", "set ioprio class (see ionice(1) manpage), "),
	OPTLINE("-n CDATA", "set ioprio classdata (see ionice(1) manpage)"),
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_scrub_start(const struct cmd_struct *cmd, int argc, char **argv)
{
	return scrub_start(cmd, argc, argv, false);
}
static DEFINE_SIMPLE_COMMAND(scrub_start, "start");

static const char * const cmd_scrub_cancel_usage[] = {
	"btrfs scrub cancel <path>|<device>",
	"Cancel a running scrub",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_scrub_cancel(const struct cmd_struct *cmd, int argc, char **argv)
{
	char *path;
	int ret;
	int fdmnt = -1;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	path = argv[optind];

	fdmnt = btrfs_open_mnt(path);
	if (fdmnt < 0) {
		ret = 1;
		goto out;
	}

	ret = ioctl(fdmnt, BTRFS_IOC_SCRUB_CANCEL, NULL);

	if (ret < 0) {
		error("scrub cancel failed on %s: %s", path,
			errno == ENOTCONN ? "not running" : strerror(errno));
		if (errno == ENOTCONN)
			ret = 2;
		else
			ret = 1;
		goto out;
	}

	ret = 0;
	pr_verbose(LOG_DEFAULT, "scrub cancelled\n");

out:
	close(fdmnt);
	return ret;
}
static DEFINE_SIMPLE_COMMAND(scrub_cancel, "cancel");

static const char * const cmd_scrub_resume_usage[] = {
	"btrfs scrub resume [-BdqrR] [-c ioprio_class -n ioprio_classdata] <path>|<device>",
	"Resume previously canceled or interrupted scrub",
	"",
	OPTLINE("-B", "do not background"),
	OPTLINE("-d", "stats per device (-B only)"),
	OPTLINE("-r", "read only mode"),
	OPTLINE("-R", "raw print mode, print full data instead of summary"),
	OPTLINE("-q", "deprecated, alias for global -q option"),
	"",
	"Priority (requires IO scheduler support, not supported by mq-deadline):",
	OPTLINE("-c CLASS", "set ioprio class (see ionice(1) manpage)"),
	OPTLINE("-n CDATA", "set ioprio classdata (see ionice(1) manpage)"),
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_scrub_resume(const struct cmd_struct *cmd, int argc, char **argv)
{
	return scrub_start(cmd, argc, argv, true);
}
static DEFINE_SIMPLE_COMMAND(scrub_resume, "resume");

static const char * const cmd_scrub_status_usage[] = {
	"btrfs scrub status [-dR] <path>|<device>",
	"Show status of running or finished scrub",
	"",
	OPTLINE("-d", "stats per device"),
	OPTLINE("-R", "print raw stats"),
	HELPINFO_UNITS_LONG,
	NULL
};

static int cmd_scrub_status(const struct cmd_struct *cmd, int argc, char **argv)
{
	char *path;
	struct btrfs_ioctl_fs_info_args fi_args;
	struct btrfs_ioctl_dev_info_args *di_args = NULL;
	struct btrfs_ioctl_space_args *si_args = NULL;
	struct scrub_file_record **past_scrubs = NULL;
	struct scrub_file_record *last_scrub;
	struct scrub_fs_stat fs_stat;
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
	};
	int in_progress;
	int ret;
	int i;
	int fdmnt;
	bool print_raw = false;
	bool do_stats_per_dev = false;
	int c;
	char fsid[BTRFS_UUID_UNPARSED_SIZE];
	int fdres = -1;
	int err = 0;

	unit_mode = get_unit_mode_from_arg(&argc, argv, 0);

	optind = 0;
	while ((c = getopt(argc, argv, "dR")) != -1) {
		switch (c) {
		case 'd':
			do_stats_per_dev = true;
			break;
		case 'R':
			print_raw = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		return 1;

	path = argv[optind];

	fdmnt = btrfs_open_mnt(path);
	if (fdmnt < 0)
		return 1;

	ret = get_fs_info(path, &fi_args, &di_args);
	if (ret) {
		errno = -ret;
		error("getting dev info for scrub failed: %m");
		err = 1;
		goto out;
	}
	if (!fi_args.num_devices) {
		error("no devices found");
		err = 1;
		goto out;
	}
	ret = get_df(fdmnt, &si_args);
	if (ret) {
		errno = -ret;
		error("cannot get space info: %m");
		err = 1;
		goto out;
	}

	uuid_unparse(fi_args.fsid, fsid);

	fdres = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fdres == -1) {
		error("failed to create socket to receive progress information: %m");
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
			errno = -fdres;
			warning("failed to open status file: %m");
			err = 1;
			goto out;
		}
	}

	if (fdres >= 0) {
		past_scrubs = scrub_read_file(fdres, 1);
		if (IS_ERR(past_scrubs)) {
			errno = -PTR_ERR(past_scrubs);
			warning("failed to read status: %m");
		}
	}
	in_progress = is_scrub_running_in_kernel(fdmnt, di_args, fi_args.num_devices);

	pr_verbose(LOG_DEFAULT, "UUID:             %s\n", fsid);

	if (do_stats_per_dev) {
		for (i = 0; i < fi_args.num_devices; ++i) {
			u64 limit;

			limit = read_scrub_device_limit(fdmnt, di_args[i].devid);
			last_scrub = last_dev_scrub(past_scrubs,
							di_args[i].devid);
			if (!last_scrub) {
				print_scrub_dev(&di_args[i], NULL, print_raw,
						NULL, NULL, limit);
				continue;
			}
			last_scrub->stats.in_progress = in_progress;
			print_scrub_dev(&di_args[i], &last_scrub->p, print_raw,
					last_scrub->stats.finished ?
							"history" : "status",
					&last_scrub->stats, limit);
		}
	} else {
		u64 total_bytes_used = 0;
		struct btrfs_ioctl_space_info *sp = si_args->spaces;
		u64 limit = 0;

		init_fs_stat(&fs_stat);
		fs_stat.s.in_progress = in_progress;
		for (i = 0; i < fi_args.num_devices; ++i) {
			/* On a multi-device filesystem, keep the lowest limit only. */
			u64 this_limit = read_scrub_device_limit(fdmnt, di_args[i].devid);
			if (!limit || (this_limit && this_limit < limit))
				limit = this_limit;

			last_scrub = last_dev_scrub(past_scrubs,
							di_args[i].devid);
			if (!last_scrub)
				continue;
			add_to_fs_stat(&last_scrub->p, &last_scrub->stats,
					&fs_stat);
		}
		for (i = 0; i < si_args->total_spaces; i++, sp++) {
			const int index = btrfs_bg_flags_to_raid_index(sp->flags);
			const int factor = btrfs_raid_array[index].ncopies;

			/* This is still slightly off for RAID56 */
			total_bytes_used += sp->used_bytes * factor;
		}
		print_fs_stat(&fs_stat, print_raw, total_bytes_used,
			      fi_args.num_devices, limit);
	}

out:
	free_history(past_scrubs);
	free(di_args);
	free(si_args);
	if (fdres > -1)
		close(fdres);
	close(fdmnt);

	return !!err;
}
static DEFINE_SIMPLE_COMMAND(scrub_status, "status");

static const char * const cmd_scrub_limit_usage[] = {
	"btrfs scrub limit [options] <path>",
	"Show or set scrub limits on devices of the given filesystem.",
	"",
	OPTLINE("-a|--all", "apply the limit to all devices"),
	OPTLINE("-d|--devid DEVID", "select the device by DEVID to apply the limit"),
	OPTLINE("-l|--limit SIZE", "set the limit of the device to SIZE (size units with suffix), or 0 to reset to unlimited"),
	HELPINFO_UNITS_LONG,
	NULL
};

static int cmd_scrub_limit(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct btrfs_ioctl_fs_info_args fi_args = { 0 };
	char fsid[BTRFS_UUID_UNPARSED_SIZE];
	struct string_table *table = NULL;
	int ret;
	int fd = -1;
	int cols, idx;
	u64 opt_devid = 0;
	bool devid_set = false;
	u64 opt_limit = 0;
	bool limit_set = false;
	bool all_set = false;

	unit_mode = get_unit_mode_from_arg(&argc, argv, 0);

	optind = 0;
	while (1) {
		int c;
		static const struct option long_options[] = {
			{ "all", no_argument, NULL, 'a' },
			{ "devid", required_argument, NULL, 'd' },
			{ "limit", required_argument, NULL, 'l' },
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "ad:l:", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'a':
			all_set = true;
			break;
		case 'd':
			opt_devid = arg_strtou64(optarg);
			devid_set = true;
			break;
		case 'l':
			opt_limit = arg_strtou64_with_suffix(optarg);
			limit_set = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}
	if (check_argc_exact(argc - optind, 1))
		return 1;

	if (devid_set && all_set) {
		error("--all and --devid cannot be used at the same time");
		return 1;
	}

	if (devid_set && !limit_set) {
		error("--devid and --limit must be set together");
		return 1;
	}
	if (all_set && !limit_set) {
		error("--all and --limit must be set together");
		return 1;
	}
	if (!all_set && !devid_set && limit_set) {
		error("--limit must be used with either --all or --devid");
		return 1;
	}

	fd = btrfs_open_file_or_dir(argv[optind]);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_FS_INFO, &fi_args);
	if (ret < 0) {
		error("failed to read filesystem info: %m");
		ret = 1;
		goto out;
	}
	if (fi_args.num_devices == 0) {
		error("no devices found");
		ret = 1;
		goto out;
	}
	uuid_unparse(fi_args.fsid, fsid);
	pr_verbose(LOG_DEFAULT, "UUID: %s\n", fsid);

	if (devid_set) {
		/* Set one device only. */
		struct btrfs_ioctl_dev_info_args di_args = { 0 };
		u64 limit;

		ret = device_get_info(fd, opt_devid, &di_args);
		if (ret == -ENODEV) {
			error("device with devid %llu not found", opt_devid);
			ret = 1;
			goto out;
		}
		limit = read_scrub_device_limit(fd, opt_devid);
		pr_verbose(LOG_DEFAULT, "Set scrub limit of devid %llu from %s%s to %s%s\n",
			   opt_devid,
			   limit > 0 ? pretty_size_mode(limit, unit_mode) : "unlimited",
			   limit > 0 ? "/s" : "",
			   opt_limit > 0 ? pretty_size_mode(opt_limit, unit_mode) : "unlimited",
			   opt_limit > 0 ? "/s" : "");
		ret = write_scrub_device_limit(fd, opt_devid, opt_limit);
		if (ret < 0) {
			errno = -ret;
			error("cannot write to the sysfs file: %m");
			ret = 1;
		}
		ret = 0;
		goto out;
	}

	if (all_set && limit_set) {
		/* Set on all devices. */
		for (u64 devid = 1; devid <= fi_args.max_id; devid++) {
			u64 limit;
			struct btrfs_ioctl_dev_info_args di_args = { 0 };

			ret = device_get_info(fd, devid, &di_args);
			if (ret == -ENODEV) {
				continue;
			} else if (ret < 0) {
				errno = -ret;
				error("cannot read devid %llu info: %m", devid);
				goto out;
			}
			limit = read_scrub_device_limit(fd, di_args.devid);
			pr_verbose(LOG_DEFAULT, "Set scrub limit of devid %llu from %s%s to %s%s\n",
				   devid,
				   limit > 0 ? pretty_size_mode(limit, unit_mode) : "unlimited",
				   limit > 0 ? "/s" : "",
				   opt_limit > 0 ? pretty_size_mode(opt_limit, unit_mode) : "unlimited",
				   opt_limit > 0 ? "/s" : "");
			ret = write_scrub_device_limit(fd, devid, opt_limit);
			if (ret < 0) {
				error("cannot write to the sysfs file of devid %llu: %m", devid);
				goto out;
			}
		}
		ret = 0;
		goto out;
	}

	cols = 3;
	table = table_create(cols, 2 + fi_args.num_devices);
	if (!table) {
		error_mem(NULL);
		ret = 1;
		goto out;
	}
	table->spacing = STRING_TABLE_SPACING_2;
	idx = 0;
	table_printf(table, idx++, 0, ">Id");
	table_printf(table, idx++, 0, ">Limit");
	table_printf(table, idx++, 0, ">Path");
	for (int i = 0; i < cols; i++)
	     table_printf(table, i, 1, "*-");

	for (u64 devid = 1, i = 0; devid <= fi_args.max_id; devid++) {
		u64 limit;
		struct btrfs_ioctl_dev_info_args di_args = { 0 };

		ret = device_get_info(fd, devid, &di_args);
		if (ret == -ENODEV) {
			continue;
		} else if (ret < 0) {
			errno = -ret;
			error("cannot read devid %llu info: %m", devid);
			goto out;
		}

		limit = read_scrub_device_limit(fd, di_args.devid);
		idx = 0;
		table_printf(table, idx++, 2 + i, ">%llu", di_args.devid);
		if (limit > 0) {
			table_printf(table, idx++, 2 + i, ">%s",
				     pretty_size_mode(limit, unit_mode));
		} else {
			table_printf(table, idx++, 2 + i, ">%s", "-");
		}
		table_printf(table, idx++, 2 + i, "<%s", di_args.path);
		i++;
	}
	table_dump(table);

out:
	if (table)
		table_free(table);
	close(fd);

	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(scrub_limit, "limit");

static const char scrub_cmd_group_info[] =
"verify checksums of data and metadata";

static const struct cmd_group scrub_cmd_group = {
	scrub_cmd_group_usage, scrub_cmd_group_info, {
		&cmd_struct_scrub_start,
		&cmd_struct_scrub_cancel,
		&cmd_struct_scrub_resume,
		&cmd_struct_scrub_status,
		&cmd_struct_scrub_limit,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(scrub);
