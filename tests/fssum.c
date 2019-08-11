/*
 * Copyright (C) 2012 STRATO AG.  All rights reserved.
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <assert.h>
#include <time.h>
#include <stdint.h>
#include "tests/sha.h"

#define CS_SIZE 32
#define CHUNKS	128

#ifndef SEEK_DATA
#define SEEK_DATA 3
#define SEEK_HOLE 4
#endif

/* TODO: add hardlink recognition */
/* TODO: add xattr/acl */

struct excludes {
	char *path;
	int len;
};

typedef struct _sum {
	SHA256Context   sha;
	unsigned char	out[CS_SIZE];
} sum_t;

typedef int (*sum_file_data_t)(int fd, sum_t *dst);

int gen_manifest = 0;
int in_manifest = 0;
char *checksum = NULL;
struct excludes *excludes;
int n_excludes = 0;
int verbose = 0;
FILE *out_fp;
FILE *in_fp;

enum _flags {
	FLAG_UID,
	FLAG_GID,
	FLAG_MODE,
	FLAG_ATIME,
	FLAG_MTIME,
	FLAG_CTIME,
	FLAG_DATA,
	FLAG_OPEN_ERROR,
	FLAG_STRUCTURE,
	NUM_FLAGS
};

const char flchar[] = "ugoamcdes";
char line[65536];

int flags[NUM_FLAGS] = {1, 1, 1, 1, 1, 0, 1, 0, 0};

char *
getln(char *buf, int size, FILE *fp)
{
	char *p;
	int l;

	p = fgets(buf, size, fp);
	if (!p)
		return NULL;

	l = strlen(p);
	while(l > 0  && (p[l - 1] == '\n' || p[l - 1] == '\r'))
		p[--l] = 0;

	return p;
}

void
parse_flag(int c)
{
	int i;
	int is_upper = 0;

	if (c >= 'A' && c <= 'Z') {
		is_upper = 1;
		c += 'a' - 'A';
	}
	for (i = 0; flchar[i]; ++i) {
		if (flchar[i] == c) {
			flags[i] = is_upper ? 0 : 1;
			return;
		}
	}
	fprintf(stderr, "unrecognized flag %c\n", c);
	exit(-1);
}

void
parse_flags(char *p)
{
	while (*p)
		parse_flag(*p++);
}

void
usage(void)
{
	fprintf(stderr, "usage: fssum <options> <path>\n");
	fprintf(stderr, "  options:\n");
	fprintf(stderr, "    -f          : write out a full manifest file\n");
	fprintf(stderr, "    -w <file>   : send output to file\n");
	fprintf(stderr, "    -v          : verbose mode (debugging only)\n");
	fprintf(stderr,
		"    -r <file>   : read checksum or manifest from file\n");
	fprintf(stderr, "    -[ugoamcde] : specify which fields to include in checksum calculation.\n");
	fprintf(stderr, "         u      : include uid\n");
	fprintf(stderr, "         g      : include gid\n");
	fprintf(stderr, "         o      : include mode\n");
	fprintf(stderr, "         m      : include mtime\n");
	fprintf(stderr, "         a      : include atime\n");
	fprintf(stderr, "         c      : include ctime\n");
	fprintf(stderr, "         d      : include file data\n");
	fprintf(stderr, "         e      : include open errors (aborts otherwise)\n");
	fprintf(stderr, "         s      : include block structure (holes)\n");
	fprintf(stderr, "    -[UGOAMCDES]: exclude respective field from calculation\n");
	fprintf(stderr, "    -n          : reset all flags\n");
	fprintf(stderr, "    -N          : set all flags\n");
	fprintf(stderr, "    -x path     : exclude path when building checksum (multiple ok)\n");
	fprintf(stderr, "    -h          : this help\n\n");
	fprintf(stderr, "The default field mask is ugoamCdES. If the checksum/manifest is read from a\n");
	fprintf(stderr, "file, the mask is taken from there and the values given on the command line\n");
	fprintf(stderr, "are ignored.\n");
	exit(-1);
}

static char buf[65536];

void *
alloc(size_t sz)
{
	void *p = malloc(sz);

	if (!p) {
		fprintf(stderr, "malloc failed\n");
		exit(-1);
	}

	return p;
}

void
sum_init(sum_t *cs)
{
	SHA256Reset(&cs->sha);
}

void
sum_fini(sum_t *cs)
{
	SHA256Result(&cs->sha, cs->out);
}

void
sum_add(sum_t *cs, void *buf, int size)
{
	SHA256Input(&cs->sha, buf, size);
}

void
sum_add_sum(sum_t *dst, sum_t *src)
{
	sum_add(dst, src->out, sizeof(src->out));
}

void
sum_add_u64(sum_t *dst, uint64_t val)
{
	uint64_t v = cpu_to_le64(val);
	sum_add(dst, &v, sizeof(v));
}

void
sum_add_time(sum_t *dst, time_t t)
{
	sum_add_u64(dst, t);
}

char *
sum_to_string(sum_t *dst)
{
	int i;
	char *s = alloc(CS_SIZE * 2 + 1);

	for (i = 0; i < CS_SIZE; ++i)
		sprintf(s + i * 2, "%02x", dst->out[i]);

	return s;
}

int
sum_file_data_permissive(int fd, sum_t *dst)
{
	int ret;
	off_t pos;
	off_t old;
	int i;
	uint64_t zeros = 0;

	pos = lseek(fd, 0, SEEK_CUR);
	if (pos == (off_t)-1)
		return errno == ENXIO ? 0 : -2;

	while (1) {
		old = pos;
		pos = lseek(fd, pos, SEEK_DATA);
		if (pos == (off_t)-1) {
			if (errno == ENXIO) {
				ret = 0;
				pos = lseek(fd, 0, SEEK_END);
				if (pos != (off_t)-1)
					zeros += pos - old;
			} else {
				ret = -2;
			}
			break;
		}
		ret = read(fd, buf, sizeof(buf));
		assert(ret); /* eof found by lseek */
		if (ret <= 0)
			break;
		if (old < pos) /* hole */
			zeros += pos - old;
		for (i = 0; i < ret; ++i) {
			for (old = i; buf[i] == 0 && i < ret; ++i)
				;
			if (old < i) /* code like a hole */
				zeros += i - old;
			if (i == ret)
				break;
			if (zeros) {
				if (verbose >= 2)
					fprintf(stderr,
						"adding %llu zeros to sum\n",
						(unsigned long long)zeros);
				sum_add_u64(dst, 0);
				sum_add_u64(dst, zeros);
				zeros = 0;
			}
			for (old = i; buf[i] != 0 && i < ret; ++i)
				;
			if (verbose >= 2)
				fprintf(stderr, "adding %d non-zeros to sum\n",
					i - (int)old);
			sum_add(dst, buf + old, i - old);
		}
		pos += ret;
	}

	if (zeros) {
		if (verbose >= 2)
			fprintf(stderr,
				"adding %llu zeros to sum (finishing)\n",
				(unsigned long long)zeros);
		sum_add_u64(dst, 0);
		sum_add_u64(dst, zeros);
	}

	return ret;
}

int
sum_file_data_strict(int fd, sum_t *dst)
{
	int ret;
	off_t pos;

	pos = lseek(fd, 0, SEEK_CUR);
	if (pos == (off_t)-1)
		return errno == ENXIO ? 0 : -2;

	while (1) {
		pos = lseek(fd, pos, SEEK_DATA);
		if (pos == (off_t)-1)
			return errno == ENXIO ? 0 : -2;
		ret = read(fd, buf, sizeof(buf));
		assert(ret); /* eof found by lseek */
		if (ret <= 0)
			return ret;
		if (verbose >= 2)
			fprintf(stderr,
				"adding to sum at file offset %llu, %d bytes\n",
				(unsigned long long)pos, ret);
		sum_add_u64(dst, (uint64_t)pos);
		sum_add(dst, buf, ret);
		pos += ret;
	}
}

char *
escape(char *in)
{
	char *out = alloc(strlen(in) * 3 + 1);
	char *src = in;
	char *dst = out;

	for (; *src; ++src) {
		if (*src >= 32 && *src < 127 && *src != '\\') {
			*dst++ = *src;
		} else {
			sprintf(dst, "\\%02x", (unsigned char)*src);
			dst += 3;
		}
	}
	*dst = 0;

	return out;
}

void
excess_file(const char *fn)
{
	printf("only in local fs: %s\n", fn);
}

void
missing_file(const char *fn)
{
	printf("only in remote fs: %s\n", fn);
}

int
pathcmp(const char *a, const char *b)
{
	int len_a = strlen(a);
	int len_b = strlen(b);

	/*
	 * as the containing directory is sent after the files, it has to
	 * come out bigger in the comparison.
	 */
	if (len_a < len_b && a[len_a - 1] == '/' && strncmp(a, b, len_a) == 0)
		return 1;
	if (len_a > len_b && b[len_b - 1] == '/' && strncmp(a, b, len_b) == 0)
		return -1;

	return strcmp(a, b);
}

void
check_match(char *fn, char *local_m, char *remote_m,
	    char *local_c, char *remote_c)
{
	int match_m = !strcmp(local_m, remote_m);
	int match_c = !strcmp(local_c, remote_c);

	if (match_m && !match_c) {
		printf("data mismatch in %s\n", fn);
	} else if (!match_m && match_c) {
		printf("metadata mismatch in %s\n", fn);
	} else if (!match_m && !match_c) {
		printf("metadata and data mismatch in %s\n", fn);
	}
}

char *prev_fn;
char *prev_m;
char *prev_c;
void
check_manifest(char *fn, char *m, char *c, int last_call)
{
	char *rem_m;
	char *rem_c;
	char *l;
	int cmp;

	if (prev_fn) {
		if (last_call)
			cmp = -1;
		else
			cmp = pathcmp(prev_fn, fn);
		if (cmp > 0) {
			excess_file(fn);
			return;
		} else if (cmp < 0) {
			missing_file(prev_fn);
		} else {
			check_match(fn, m, prev_m, c, prev_c);
		}
		free(prev_fn);
		free(prev_m);
		free(prev_c);
		prev_fn = NULL;
		prev_m = NULL;
		prev_c = NULL;
		if (cmp == 0)
			return;
	}
	while ((l = getln(line, sizeof(line), in_fp))) {
		rem_c = strrchr(l, ' ');
		if (!rem_c) {
			if (checksum)
				free(checksum);

			/* final cs */
			checksum = strdup(l);
			break;
		}
		if (rem_c == l) {
malformed:
			fprintf(stderr, "malformed input\n");
			exit(-1);
		}
		*rem_c++ = 0;
		rem_m = strrchr(l, ' ');
		if (!rem_m)
			goto malformed;
		*rem_m++ = 0;

		if (last_call)
			cmp = -1;
		else
			cmp = pathcmp(l, fn);
		if (cmp == 0) {
			check_match(fn, m, rem_m, c, rem_c);
			return;
		} else if (cmp > 0) {
			excess_file(fn);
			prev_fn = strdup(l);
			prev_m = strdup(rem_m);
			prev_c = strdup(rem_c); 
			return;
		}
		missing_file(l);
	}
	if (!last_call)
		excess_file(fn);
}

int
namecmp(const void *aa, const void *bb)
{
	char * const *a = aa;
	char * const *b = bb;

	return strcmp(*a, *b);
}

void
sum(int dirfd, int level, sum_t *dircs, char *path_prefix, char *path_in)
{
	DIR *d;
	struct dirent *de;
	char **namelist = NULL;
	int alloclen = 0;
	int entries = 0;
	int i;
	int ret;
	int fd;
	int excl;
	sum_file_data_t sum_file_data = flags[FLAG_STRUCTURE] ?
			sum_file_data_strict : sum_file_data_permissive;

	d = fdopendir(dirfd);
	if (!d) {
		perror("opendir");
		exit(-1);
	}
	while((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (entries == alloclen) {
			alloclen += CHUNKS;
			namelist = realloc(namelist,
					   alloclen * sizeof(*namelist));
			if (!namelist) {
				fprintf(stderr, "malloc failed\n");
				exit(-1);
			}
		}
		namelist[entries] = strdup(de->d_name);
		if (!namelist[entries]) {
			fprintf(stderr, "malloc failed\n");
			exit(-1);
		}
		++entries;
	}

	qsort(namelist, entries, sizeof(*namelist), namecmp);
	for (i = 0; i < entries; ++i) {
		struct stat64 st;
		sum_t cs;
		sum_t meta;
		char *path;

		sum_init(&cs);
		sum_init(&meta);
		path = alloc(strlen(path_in) + strlen(namelist[i]) + 3);
		sprintf(path, "%s/%s", path_in, namelist[i]);
		for (excl = 0; excl < n_excludes; ++excl) {
			if (strncmp(excludes[excl].path, path,
			    excludes[excl].len) == 0)
				goto next;
		}

		ret = fchdir(dirfd);
		if (ret == -1) {
			perror("fchdir");
			exit(-1);
		}
		ret = lstat64(namelist[i], &st);
		if (ret) {
			fprintf(stderr, "stat failed for %s/%s: %m\n",
				path_prefix, path);
			exit(-1);
		}
		sum_add_u64(&meta, level);
		sum_add(&meta, namelist[i], strlen(namelist[i]));
		if (!S_ISDIR(st.st_mode))
			sum_add_u64(&meta, st.st_nlink);
		if (flags[FLAG_UID])
			sum_add_u64(&meta, st.st_uid);
		if (flags[FLAG_GID])
			sum_add_u64(&meta, st.st_gid);
		if (flags[FLAG_MODE])
			sum_add_u64(&meta, st.st_mode);
		if (flags[FLAG_ATIME])
			sum_add_time(&meta, st.st_atime);
		if (flags[FLAG_MTIME])
			sum_add_time(&meta, st.st_mtime);
		if (flags[FLAG_CTIME])
			sum_add_time(&meta, st.st_ctime);
		if (S_ISDIR(st.st_mode)) {
			fd = openat(dirfd, namelist[i], 0);
			if (fd == -1 && flags[FLAG_OPEN_ERROR]) {
				sum_add_u64(&meta, errno);
			} else if (fd == -1) {
				fprintf(stderr, "open failed for %s/%s: %m\n",
					path_prefix, path);
				exit(-1);
			} else {
				sum(fd, level + 1, &cs, path_prefix, path);
				close(fd);
			}
		} else if (S_ISREG(st.st_mode)) {
			sum_add_u64(&meta, st.st_size);
			if (flags[FLAG_DATA]) {
				if (verbose)
					fprintf(stderr, "file %s\n",
						namelist[i]);
				fd = openat(dirfd, namelist[i], 0);
				if (fd == -1 && flags[FLAG_OPEN_ERROR]) {
					sum_add_u64(&meta, errno);
				} else if (fd == -1) {
					fprintf(stderr,
						"open failed for %s/%s: %m\n",
						path_prefix, path);
					exit(-1);
				}
				if (fd != -1) {
					ret = sum_file_data(fd, &cs);
					if (ret < 0) {
						fprintf(stderr,
							"read failed for "
							"%s/%s: %m\n",
							path_prefix, path);
						exit(-1);
					}
					close(fd);
				}
			}
		} else if (S_ISLNK(st.st_mode)) {
			ret = readlink(namelist[i], buf, sizeof(buf));
			if (ret == -1) {
				perror("readlink");
				exit(-1);
			}
			sum_add(&cs, buf, ret);
		} else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
			sum_add_u64(&cs, major(st.st_rdev));
			sum_add_u64(&cs, minor(st.st_rdev));
		}
		sum_fini(&cs);
		sum_fini(&meta);
		if (gen_manifest || in_manifest) {
			char *fn;
			char *m;
			char *c;

			if (S_ISDIR(st.st_mode))
				strcat(path, "/");
			fn = escape(path);
			m = sum_to_string(&meta);
			c = sum_to_string(&cs);

			if (gen_manifest)
				fprintf(out_fp, "%s %s %s\n", fn, m, c);
			if (in_manifest)
				check_manifest(fn, m, c, 0);
			free(c);
			free(m);
			free(fn);
		}
		sum_add_sum(dircs, &cs);
		sum_add_sum(dircs, &meta);
next:
		free(path);
		free(namelist[i]);
	}

	free(namelist);
	closedir(d);
}

int
main(int argc, char *argv[])
{
	extern char *optarg;
	extern int optind;
	int	c;
	char *path;
	int fd;
	sum_t cs;
	char *sumstring;
	char flagstring[sizeof(flchar)];
	int ret = 0;
	int i;
	int plen;
	int elen;
	int n_flags = 0;
	const char *allopts = "heEfuUgGoOaAmMcCdDsSnNw:r:vx:";

	out_fp = stdout;
	while ((c = getopt(argc, argv, allopts)) != EOF) {
		switch(c) {
		case 'f':
			gen_manifest = 1;
			break;
		case 'u':
		case 'U':
		case 'g':
		case 'G':
		case 'o':
		case 'O':
		case 'a':
		case 'A':
		case 'm':
		case 'M':
		case 'c':
		case 'C':
		case 'd':
		case 'D':
		case 'e':
		case 'E':
		case 's':
		case 'S':
			++n_flags;
			parse_flag(c);
			break;
		case 'n':
			for (i = 0; i < NUM_FLAGS; ++i)
				flags[i] = 0;
			break;
		case 'N':
			for (i = 0; i < NUM_FLAGS; ++i)
				flags[i] = 1;
			break;
		case 'w':
			out_fp = fopen(optarg, "w");
			if (!out_fp) {
				fprintf(stderr,
					"failed to open output file: %m\n");
				exit(-1);
			}
			break;
		case 'r':
			in_fp = fopen(optarg, "r");
			if (!in_fp) {
				fprintf(stderr,
					"failed to open input file: %m\n");
				exit(-1);
			}
			break;
		case 'x':
			++n_excludes;
			excludes = realloc(excludes,
					   sizeof(*excludes) * n_excludes);
			if (!excludes) {
				fprintf(stderr,
					"failed to alloc exclude space\n");
				exit(-1);
			}
			excludes[n_excludes - 1].path = optarg;
			break;
		case 'v':
			++verbose;
			break;
		case 'h':
		case '?':
			usage();
		}
	}

	if (optind + 1 != argc) {
		fprintf(stderr, "missing path\n");
		usage();
	}

	if (in_fp) {
		char *l = getln(line, sizeof(line), in_fp);
		char *p;

		if (l == NULL) {
			fprintf(stderr, "failed to read line from input\n");
			exit(-1);
		}
		if (strncmp(l, "Flags: ", 7) == 0) {
			l += 7;
			in_manifest = 1;
			parse_flags(l);
		} else if ((p = strchr(l, ':'))) {
			*p++ = 0;
			parse_flags(l);

			if (checksum)
				free(checksum);
			checksum = strdup(p);
		} else {
			fprintf(stderr, "invalid input file format\n");
			exit(-1);
		}
		if (n_flags)
			fprintf(stderr, "warning: "
				"command line flags ignored in -r mode\n");
	}
	strcpy(flagstring, flchar);
	for (i = 0; i < NUM_FLAGS; ++i) {
		if (flags[i] == 0)
			flagstring[i] -= 'a' - 'A';
	}

	path = argv[optind];
	plen = strlen(path);
	if (path[plen - 1] == '/') {
		--plen;
		path[plen] = '\0';
	}

	for (i = 0; i < n_excludes; ++i) {
		if (strncmp(path, excludes[i].path, plen) != 0)
			fprintf(stderr,
				"warning: exclude %s outside of path %s\n",
				excludes[i].path, path);
		else
			excludes[i].path += plen;
		elen = strlen(excludes[i].path);
		if (excludes[i].path[elen - 1] == '/')
			--elen;
		excludes[i].path[elen] = '\0';
		excludes[i].len = elen;
	}

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		fprintf(stderr, "failed to open %s: %m\n", path);
		exit(-1);
	}

	if (gen_manifest)
		fprintf(out_fp, "Flags: %s\n", flagstring);

	sum_init(&cs);
	sum(fd, 1, &cs, path, "");
	sum_fini(&cs);

	close(fd);
	if (in_manifest)
		check_manifest("", "", "", 1);

	if (!checksum) {
		if (in_manifest) {
			fprintf(stderr, "malformed input\n");
			exit(-1);
		}
		if (!gen_manifest)
			fprintf(out_fp, "%s:", flagstring);

		sumstring = sum_to_string(&cs);
		fprintf(out_fp, "%s\n", sumstring);
		free(sumstring);
	} else {
		sumstring = sum_to_string(&cs);
		if (strcmp(checksum, sumstring) == 0) {
			printf("OK\n");
			ret = 0;
		} else {
			printf("FAIL\n");
			ret = 1;
		}

		free(checksum);
		free(sumstring);
	}

	if (in_fp)
		fclose(in_fp);

	if (out_fp != stdout)
		fclose(out_fp);

	exit(ret);
}
