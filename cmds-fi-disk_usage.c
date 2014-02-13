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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdarg.h>

#include "utils.h"
#include "kerncompat.h"
#include "ctree.h"
#include "string_table.h"

#include "commands.h"

#include "version.h"

#define DF_HUMAN_UNIT		(1<<0)

/*
 * To store the size information about the chunks:
 * the chunks info are grouped by the tuple (type, devid, num_stripes),
 * i.e. if two chunks are of the same type (RAID1, DUP...), are on the
 * same disk, have the same stripes then their sizes are grouped
 */
struct chunk_info {
	u64	type;
	u64	size;
	u64	devid;
	u64	num_stripes;
};

/* to store information about the disks */
struct disk_info {
	u64	devid;
	char	path[BTRFS_DEVICE_PATH_NAME_MAX];
	u64	size;
};

/*
 * Pretty print the size
 * PAY ATTENTION: it return a statically buffer
 */
static char *df_pretty_sizes(u64 size, int mode)
{
	static char buf[30];

	if (mode & DF_HUMAN_UNIT)
		(void)pretty_size_snprintf(size, buf, sizeof(buf));
	else
		sprintf(buf, "%llu", size);

	return buf;
}

/*
 * Add the chunk info to the chunk_info list
 */
static int add_info_to_list(struct chunk_info **info_ptr,
			int *info_count,
			struct btrfs_chunk *chunk)
{

	u64 type = btrfs_stack_chunk_type(chunk);
	u64 size = btrfs_stack_chunk_length(chunk);
	int num_stripes = btrfs_stack_chunk_num_stripes(chunk);
	int j;

	for (j = 0 ; j < num_stripes ; j++) {
		int i;
		struct chunk_info *p = 0;
		struct btrfs_stripe *stripe;
		u64    devid;

		stripe = btrfs_stripe_nr(chunk, j);
		devid = btrfs_stack_stripe_devid(stripe);

		for (i = 0 ; i < *info_count ; i++)
			if ((*info_ptr)[i].type == type &&
			    (*info_ptr)[i].devid == devid &&
			    (*info_ptr)[i].num_stripes == num_stripes ) {
				p = (*info_ptr) + i;
				break;
			}

		if (!p) {
			int size = sizeof(struct btrfs_chunk) * (*info_count+1);
			struct chunk_info *res = realloc(*info_ptr, size);

			if (!res) {
				fprintf(stderr, "ERROR: not enough memory\n");
				return -1;
			}

			*info_ptr = res;
			p = res + *info_count;
			(*info_count)++;

			p->devid = devid;
			p->type = type;
			p->size = 0;
			p->num_stripes = num_stripes;
		}

		p->size += size;

	}

	return 0;

}

/*
 *  Helper to sort the chunk type
 */
static int cmp_chunk_block_group(u64 f1, u64 f2)
{

	u64 mask;

	if ((f1 & BTRFS_BLOCK_GROUP_TYPE_MASK) ==
		(f2 & BTRFS_BLOCK_GROUP_TYPE_MASK))
			mask = BTRFS_BLOCK_GROUP_PROFILE_MASK;
	else if (f2 & BTRFS_BLOCK_GROUP_SYSTEM)
			return -1;
	else if (f1 & BTRFS_BLOCK_GROUP_SYSTEM)
			return +1;
	else
			mask = BTRFS_BLOCK_GROUP_TYPE_MASK;

	if ((f1 & mask) > (f2 & mask))
		return +1;
	else if ((f1 & mask) < (f2 & mask))
		return -1;
	else
		return 0;
}

/*
 * Helper to sort the chunk
 */
static int cmp_chunk_info(const void *a, const void *b)
{
	return cmp_chunk_block_group(
		((struct chunk_info *)a)->type,
		((struct chunk_info *)b)->type);
}

/*
 * This function load all the chunk info from the 'fd' filesystem
 */
static int load_chunk_info(int fd,
			  struct chunk_info **info_ptr,
			  int *info_count)
{

	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header *sh;
	unsigned long off = 0;
	int i, e;


	memset(&args, 0, sizeof(args));

	/*
	 * there may be more than one ROOT_ITEM key if there are
	 * snapshots pending deletion, we have to loop through
	 * them.
	 */


	sk->tree_id = BTRFS_CHUNK_TREE_OBJECTID;

	sk->min_objectid = 0;
	sk->max_objectid = (u64)-1;
	sk->max_type = 0;
	sk->min_type = (u8)-1;
	sk->min_offset = 0;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 4096;

	while (1) {
		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
		e = errno;
		if (ret < 0) {
			fprintf(stderr,
				"ERROR: can't perform the search - %s\n",
				strerror(e));
			return -99;
		}
		/* the ioctl returns the number of item it found in nr_items */

		if (sk->nr_items == 0)
			break;

		off = 0;
		for (i = 0; i < sk->nr_items; i++) {
			struct btrfs_chunk *item;
			sh = (struct btrfs_ioctl_search_header *)(args.buf +
								  off);

			off += sizeof(*sh);
			item = (struct btrfs_chunk *)(args.buf + off);

			if (add_info_to_list(info_ptr, info_count, item)) {
				*info_ptr = 0;
				free(*info_ptr);
				return -100;
			}

			off += sh->len;

			sk->min_objectid = sh->objectid;
			sk->min_type = sh->type;
			sk->min_offset = sh->offset+1;

		}
		if (!sk->min_offset)	/* overflow */
			sk->min_type++;
		else
			continue;

		if (!sk->min_type)
			sk->min_objectid++;
		 else
			continue;

		if (!sk->min_objectid)
			break;
	}

	qsort(*info_ptr, *info_count, sizeof(struct chunk_info),
		cmp_chunk_info);

	return 0;

}

/*
 * Helper to sort the struct btrfs_ioctl_space_info
 */
static int cmp_btrfs_ioctl_space_info(const void *a, const void *b)
{
	return cmp_chunk_block_group(
		((struct btrfs_ioctl_space_info *)a)->flags,
		((struct btrfs_ioctl_space_info *)b)->flags);
}

/*
 * This function load all the information about the space usage
 */
static struct btrfs_ioctl_space_args *load_space_info(int fd, char *path)
{
	struct btrfs_ioctl_space_args *sargs = 0, *sargs_orig = 0;
	int e, ret, count;

	sargs_orig = sargs = malloc(sizeof(struct btrfs_ioctl_space_args));
	if (!sargs) {
		fprintf(stderr, "ERROR: not enough memory\n");
		return NULL;
	}

	sargs->space_slots = 0;
	sargs->total_spaces = 0;

	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	e = errno;
	if (ret) {
		fprintf(stderr,
			"ERROR: couldn't get space info on '%s' - %s\n",
			path, strerror(e));
		free(sargs);
		return NULL;
	}
	if (!sargs->total_spaces) {
		free(sargs);
		printf("No chunks found\n");
		return NULL;
	}

	count = sargs->total_spaces;

	sargs = realloc(sargs, sizeof(struct btrfs_ioctl_space_args) +
			(count * sizeof(struct btrfs_ioctl_space_info)));
	if (!sargs) {
		free(sargs_orig);
		fprintf(stderr, "ERROR: not enough memory\n");
		return NULL;
	}

	sargs->space_slots = count;
	sargs->total_spaces = 0;

	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, sargs);
	e = errno;

	if (ret) {
		fprintf(stderr,
			"ERROR: couldn't get space info on '%s' - %s\n",
			path, strerror(e));
		free(sargs);
		return NULL;
	}

	qsort(&(sargs->spaces), count, sizeof(struct btrfs_ioctl_space_info),
		cmp_btrfs_ioctl_space_info);

	return sargs;
}

/*
 * This function computes the space occuped by a *single* RAID5/RAID6 chunk.
 * The computation is performed on the basis of the number of stripes
 * which compose the chunk, which could be different from the number of disks
 * if a disk is added later.
 */
static int get_raid56_used(int fd, u64 *raid5_used, u64 *raid6_used)
{
	struct chunk_info *info_ptr=0, *p;
	int info_count=0;
	int ret;

	*raid5_used = *raid6_used =0;

	ret = load_chunk_info(fd, &info_ptr, &info_count);
	if( ret < 0)
		return ret;

	for ( p = info_ptr; info_count ; info_count--, p++ ) {
		if (p->type & BTRFS_BLOCK_GROUP_RAID5)
			(*raid5_used) += p->size / (p->num_stripes -1);
		if (p->type & BTRFS_BLOCK_GROUP_RAID6)
			(*raid6_used) += p->size / (p->num_stripes -2);
	}

	return 0;

}

static int _cmd_disk_free(int fd, char *path, int mode)
{
	struct btrfs_ioctl_space_args *sargs = 0;
	int i;
	int ret = 0;
	int e, width;
	u64 total_disk;		/* filesystem size == sum of
				   disks sizes */
	u64 total_chunks;	/* sum of chunks sizes on disk(s) */
	u64 total_used;		/* logical space used */
	u64 total_free;		/* logical space un-used */
	double K;
	u64 raid5_used, raid6_used;

	if ((sargs = load_space_info(fd, path)) == NULL) {
		ret = -1;
		goto exit;
	}

	total_disk = disk_size(path);
	e = errno;
	if (total_disk == 0) {
		fprintf(stderr,
			"ERROR: couldn't get space info on '%s' - %s\n",
			path, strerror(e));

		ret = 19;
		goto exit;
	}
	if (get_raid56_used(fd, &raid5_used, &raid6_used) < 0) {
		fprintf(stderr,
			"ERROR: couldn't get space info on '%s'\n",
			path );
		ret = 20;
		goto exit;
	}

	total_chunks = total_used = total_free = 0;

	for (i = 0; i < sargs->total_spaces; i++) {
		float ratio = 1;
		u64 allocated;
		u64 flags = sargs->spaces[i].flags;

		/*
		 * The raid5/raid6 ratio depends by the stripes number
		 * used by every chunk. It is computed separately
		 */
		if (flags & BTRFS_BLOCK_GROUP_RAID0)
			ratio = 1;
		else if (flags & BTRFS_BLOCK_GROUP_RAID1)
			ratio = 2;
		else if (flags & BTRFS_BLOCK_GROUP_RAID5)
			ratio = 0;
		else if (flags & BTRFS_BLOCK_GROUP_RAID6)
			ratio = 0;
		else if (flags & BTRFS_BLOCK_GROUP_DUP)
			ratio = 2;
		else if (flags & BTRFS_BLOCK_GROUP_RAID10)
			ratio = 2;
		else
			ratio = 1;

		allocated = sargs->spaces[i].total_bytes * ratio;

		total_chunks += allocated;
		total_used += sargs->spaces[i].used_bytes;
		total_free += (sargs->spaces[i].total_bytes -
					sargs->spaces[i].used_bytes);

	}

	/* add the raid5/6 allocated space */
	total_chunks += raid5_used + raid6_used;

	K = ((double)total_used + (double)total_free) /	(double)total_chunks;

	if (mode & DF_HUMAN_UNIT)
		width = 10;
	else
		width = 18;

	printf("Disk size:\t\t%*s\n", width,
		df_pretty_sizes(total_disk, mode));
	printf("Disk allocated:\t\t%*s\n", width,
		df_pretty_sizes(total_chunks, mode));
	printf("Disk unallocated:\t%*s\n", width,
		df_pretty_sizes(total_disk-total_chunks, mode));
	printf("Used:\t\t\t%*s\n", width,
		df_pretty_sizes(total_used, mode));
	printf("Free (Estimated):\t%*s\t(",
		width,
		df_pretty_sizes((u64)(K*total_disk-total_used), mode));
	printf("Max: %s, ",
		df_pretty_sizes(total_disk-total_chunks+total_free, mode));
	printf("min: %s)\n",
		df_pretty_sizes((total_disk-total_chunks)/2+total_free, mode));
	printf("Data to disk ratio:\t%*.0f %%\n",
		width-2, K*100);

exit:

	if (sargs)
		free(sargs);

	return ret;
}

const char * const cmd_filesystem_df_usage[] = {
	"btrfs filesystem df [-b] <path> [<path>..]",
	"Show space usage information for a mount point(s).",
	"",
	"-b\tSet byte as unit",
	NULL
};

int cmd_filesystem_df(int argc, char **argv)
{

	int	flags = DF_HUMAN_UNIT;
	int	i, more_than_one = 0;

	optind = 1;
	while (1) {
		char	c = getopt(argc, argv, "b");
		if (c < 0)
			break;

		switch (c) {
		case 'b':
			flags &= ~DF_HUMAN_UNIT;
			break;
		default:
			usage(cmd_filesystem_df_usage);
		}
	}

	if (check_argc_min(argc - optind, 1)) {
		usage(cmd_filesystem_df_usage);
		return 21;
	}

	for (i = optind; i < argc ; i++) {
		int r, fd;
		DIR *dirstream = NULL;
		if (more_than_one)
			printf("\n");

		fd = open_file_or_dir(argv[i], &dirstream);
		if (fd < 0) {
			fprintf(stderr, "ERROR: can't access to '%s'\n",
				argv[1]);
			return 12;
		}
		r = _cmd_disk_free(fd, argv[i], flags);
		close_file_or_dir(fd, dirstream);

		if (r)
			return r;
		more_than_one = 1;

	}

	return 0;
}

/*
 *  Helper to sort the disk_info structure
 */
static int cmp_disk_info(const void *a, const void *b)
{
	return strcmp(((struct disk_info *)a)->path,
			((struct disk_info *)b)->path);
}

/*
 *  This function load the disk_info structure and put them in an array
 */
static int load_disks_info(int fd,
			   struct disk_info **disks_info_ptr,
			   int *disks_info_count)
{

	int ret, i, ndevs;
	struct btrfs_ioctl_fs_info_args fi_args;
	struct btrfs_ioctl_dev_info_args dev_info;
	struct disk_info *info;

	*disks_info_count = 0;
	*disks_info_ptr = 0;

	ret = ioctl(fd, BTRFS_IOC_FS_INFO, &fi_args);
	if (ret < 0) {
		fprintf(stderr, "ERROR: cannot get filesystem info\n");
		return -1;
	}

	info = malloc(sizeof(struct disk_info) * fi_args.num_devices);
	if (!info) {
		fprintf(stderr, "ERROR: not enough memory\n");
		return -1;
	}

	for (i = 0, ndevs = 0 ; i <= fi_args.max_id ; i++) {

		BUG_ON(ndevs >= fi_args.num_devices);
		ret = get_device_info(fd, i, &dev_info);

		if (ret == -ENODEV)
			continue;
		if (ret) {
			fprintf(stderr,
			    "ERROR: cannot get info about device devid=%d\n",
			    i);
			free(info);
			return -1;
		}

		info[ndevs].devid = dev_info.devid;
		strcpy(info[ndevs].path, (char *)dev_info.path);
		info[ndevs].size = get_partition_size((char *)dev_info.path);
		++ndevs;
	}

	BUG_ON(ndevs != fi_args.num_devices);
	qsort(info, fi_args.num_devices,
		sizeof(struct disk_info), cmp_disk_info);

	*disks_info_count = fi_args.num_devices;
	*disks_info_ptr = info;

	return 0;

}

/*
 *  This function computes the size of a chunk in a disk
 */
static u64 calc_chunk_size(struct chunk_info *ci)
{
	if (ci->type & BTRFS_BLOCK_GROUP_RAID0)
		return ci->size / ci->num_stripes;
	else if (ci->type & BTRFS_BLOCK_GROUP_RAID1)
		return ci->size ;
	else if (ci->type & BTRFS_BLOCK_GROUP_DUP)
		return ci->size ;
	else if (ci->type & BTRFS_BLOCK_GROUP_RAID5)
		return ci->size / (ci->num_stripes -1);
	else if (ci->type & BTRFS_BLOCK_GROUP_RAID6)
		return ci->size / (ci->num_stripes -2);
	else if (ci->type & BTRFS_BLOCK_GROUP_RAID10)
		return ci->size / ci->num_stripes;
	return ci->size;
}

/*
 *  This function print the results of the command btrfs fi disk-usage
 *  in tabular format
 */
static void _cmd_filesystem_disk_usage_tabular(int mode,
					struct btrfs_ioctl_space_args *sargs,
					struct chunk_info *chunks_info_ptr,
					int chunks_info_count,
					struct disk_info *disks_info_ptr,
					int disks_info_count)
{
	int i;
	u64 total_unused = 0;
	struct string_table *matrix = 0;
	int  ncols, nrows;

	ncols = sargs->total_spaces + 2;
	nrows = 2 + 1 + disks_info_count + 1 + 2;

	matrix = table_create(ncols, nrows);
	if (!matrix) {
		fprintf(stderr, "ERROR: not enough memory\n");
		return;
	}

	/* header */
	for (i = 0; i < sargs->total_spaces; i++) {
		const char *description;

		u64 flags = sargs->spaces[i].flags;
		description = group_type_str(flags);

		table_printf(matrix, 1+i, 0, "<%s", description);
	}

	for (i = 0; i < sargs->total_spaces; i++) {
		const char *r_mode;

		u64 flags = sargs->spaces[i].flags;
		r_mode = group_profile_str(flags);

		table_printf(matrix, 1+i, 1, "<%s", r_mode);
	}

	table_printf(matrix, 1+sargs->total_spaces, 1, "<Unallocated");

	/* body */
	for (i = 0 ; i < disks_info_count ; i++) {
		int k, col;
		char *p;

		u64  total_allocated = 0, unused;

		p = strrchr(disks_info_ptr[i].path, '/');
		if (!p)
			p = disks_info_ptr[i].path;
		else
			p++;

		table_printf(matrix, 0, i+3, "<%s",
				disks_info_ptr[i].path);

		for (col = 1, k = 0 ; k < sargs->total_spaces ; k++)  {
			u64	flags = sargs->spaces[k].flags;
			u64 devid = disks_info_ptr[i].devid;
			int	j;
			u64 size = 0;

			for (j = 0 ; j < chunks_info_count ; j++) {
				if (chunks_info_ptr[j].type != flags )
						continue;
				if (chunks_info_ptr[j].devid != devid)
						continue;

				size += calc_chunk_size(chunks_info_ptr+j);
			}

			if (size)
				table_printf(matrix, col, i+3,
					">%s", df_pretty_sizes(size, mode));
			else
				table_printf(matrix, col, i+3, ">-");

			total_allocated += size;
			col++;
		}

		unused = get_partition_size(disks_info_ptr[i].path) -
				total_allocated;

		table_printf(matrix, sargs->total_spaces + 1, i + 3,
			       ">%s", df_pretty_sizes(unused, mode));
		total_unused += unused;

	}

	for (i = 0; i <= sargs->total_spaces; i++)
		table_printf(matrix, i + 1, disks_info_count + 3, "=");


	/* footer */
	table_printf(matrix, 0, disks_info_count + 4, "<Total");
	for (i = 0; i < sargs->total_spaces; i++)
		table_printf(matrix, 1 + i, disks_info_count + 4,
			">%s",
			df_pretty_sizes(sargs->spaces[i].total_bytes, mode));

	table_printf(matrix, sargs->total_spaces+1, disks_info_count+4,
		">%s", df_pretty_sizes(total_unused, mode));

	table_printf(matrix, 0, disks_info_count+5, "<Used");
	for (i = 0; i < sargs->total_spaces; i++)
		table_printf(matrix, 1+i, disks_info_count+5, ">%s",
			df_pretty_sizes(sargs->spaces[i].used_bytes, mode));


	table_dump(matrix);
	table_free(matrix);

}

/*
 *  This function prints the unused space per every disk
 */
static void print_unused(struct chunk_info *info_ptr,
			  int info_count,
			  struct disk_info *disks_info_ptr,
			  int disks_info_count,
			  int mode)
{
	int i;
	for (i = 0 ; i < disks_info_count ; i++) {

		int	j;
		u64	total = 0;

		for (j = 0 ; j < info_count ; j++)
			if (info_ptr[j].devid == disks_info_ptr[i].devid)
				total += calc_chunk_size(info_ptr+j);

		printf("   %s\t%10s\n",
			disks_info_ptr[i].path,
			df_pretty_sizes(disks_info_ptr[i].size - total, mode));

	}

}

/*
 *  This function prints the allocated chunk per every disk
 */
static void print_chunk_disks(u64 chunk_type,
				struct chunk_info *chunks_info_ptr,
				int chunks_info_count,
				struct disk_info *disks_info_ptr,
				int disks_info_count,
				int mode)
{
	int i;

	for (i = 0 ; i < disks_info_count ; i++) {

		int	j;
		u64	total = 0;

		for (j = 0 ; j < chunks_info_count ; j++) {

			if (chunks_info_ptr[j].type != chunk_type)
				continue;
			if (chunks_info_ptr[j].devid != disks_info_ptr[i].devid)
				continue;

			total += calc_chunk_size(&(chunks_info_ptr[j]));
			//total += chunks_info_ptr[j].size;
		}

		if (total > 0)
			printf("   %s\t%10s\n",
				disks_info_ptr[i].path,
				df_pretty_sizes(total, mode));
	}
}

/*
 *  This function print the results of the command btrfs fi disk-usage
 *  in linear format
 */
static void _cmd_filesystem_disk_usage_linear(int mode,
					struct btrfs_ioctl_space_args *sargs,
					struct chunk_info *info_ptr,
					int info_count,
					struct disk_info *disks_info_ptr,
					int disks_info_count)
{
	int i;

	for (i = 0; i < sargs->total_spaces; i++) {
		const char *description;
		const char *r_mode;

		u64 flags = sargs->spaces[i].flags;
		description= group_type_str(flags);
		r_mode = group_profile_str(flags);

		printf("%s,%s: Size:%s, ",
			description,
			r_mode,
			df_pretty_sizes(sargs->spaces[i].total_bytes ,
			    mode));
		printf("Used:%s\n",
			df_pretty_sizes(sargs->spaces[i].used_bytes,
					mode));
		print_chunk_disks(flags, info_ptr, info_count,
				disks_info_ptr, disks_info_count,
				mode);
		printf("\n");

	}

	printf("Unallocated:\n");
	print_unused(info_ptr, info_count,
			disks_info_ptr, disks_info_count,
			mode);



}

static int _cmd_filesystem_disk_usage(int fd, char *path, int mode, int tabular)
{
	struct btrfs_ioctl_space_args *sargs = 0;
	int info_count = 0;
	struct chunk_info *info_ptr = 0;
	struct disk_info *disks_info_ptr = 0;
	int disks_info_count = 0;
	int ret = 0;

	if (load_chunk_info(fd, &info_ptr, &info_count) ||
	    load_disks_info(fd, &disks_info_ptr, &disks_info_count)) {
		ret = -1;
		goto exit;
	}

	if ((sargs = load_space_info(fd, path)) == NULL) {
		ret = -1;
		goto exit;
	}

	if (tabular)
		_cmd_filesystem_disk_usage_tabular(mode, sargs,
					info_ptr, info_count,
					disks_info_ptr, disks_info_count);
	else
		_cmd_filesystem_disk_usage_linear(mode, sargs,
					info_ptr, info_count,
					disks_info_ptr, disks_info_count);

exit:

	if (sargs)
		free(sargs);
	if (disks_info_ptr)
		free(disks_info_ptr);
	if (info_ptr)
		free(info_ptr);

	return ret;
}

const char * const cmd_filesystem_disk_usage_usage[] = {
	"btrfs filesystem disk-usage [-b][-t] <path> [<path>..]",
	"Show in which disk the chunks are allocated.",
	"",
	"-b\tSet byte as unit",
	"-t\tShow data in tabular format",
	NULL
};

int cmd_filesystem_disk_usage(int argc, char **argv)
{

	int	flags =	DF_HUMAN_UNIT;
	int	i, more_than_one = 0;
	int	tabular = 0;

	optind = 1;
	while (1) {
		char	c = getopt(argc, argv, "bt");
		if (c < 0)
			break;
		switch (c) {
		case 'b':
			flags &= ~DF_HUMAN_UNIT;
			break;
		case 't':
			tabular = 1;
			break;
		default:
			usage(cmd_filesystem_disk_usage_usage);
		}
	}

	if (check_argc_min(argc - optind, 1)) {
		usage(cmd_filesystem_disk_usage_usage);
		return 21;
	}

	for (i = optind; i < argc ; i++) {
		int r, fd;
		DIR	*dirstream = NULL;
		if (more_than_one)
			printf("\n");

		fd = open_file_or_dir(argv[i], &dirstream);
		if (fd < 0) {
			fprintf(stderr, "ERROR: can't access to '%s'\n",
				argv[1]);
			return 12;
		}
		r = _cmd_filesystem_disk_usage(fd, argv[i], flags, tabular);
		close_file_or_dir(fd, dirstream);

		if (r)
			return r;
		more_than_one = 1;

	}

	return 0;
}

static void print_disk_chunks(int fd,
				u64 devid,
				u64 total_size,
				struct chunk_info *chunks_info_ptr,
				int chunks_info_count,
				int mode)
{
	int i;
	u64 allocated = 0;

	for (i = 0 ; i < chunks_info_count ; i++) {
		const char *description;
		const char *r_mode;
		u64 flags;
		u64 size;

		if (chunks_info_ptr[i].devid != devid)
			continue;

		flags = chunks_info_ptr[i].type;

		description = group_type_str(flags);
		r_mode = group_profile_str(flags);
		size = calc_chunk_size(chunks_info_ptr+i);
		printf("   %s,%s:%*s%10s\n",
			description,
			r_mode,
			(int)(20 - strlen(description) - strlen(r_mode)), "",
			df_pretty_sizes(size, mode));

		allocated += size;

	}
	printf("   Unallocated: %*s%10s\n",
		(int)(20 - strlen("Unallocated")), "",
		df_pretty_sizes(total_size - allocated, mode));

}

static int _cmd_device_disk_usage(int fd, char *path, int mode)
{
	int i;
	int ret = 0;
	int info_count = 0;
	struct chunk_info *info_ptr = 0;
	struct disk_info *disks_info_ptr = 0;
	int disks_info_count = 0;

	if (load_chunk_info(fd, &info_ptr, &info_count) ||
	    load_disks_info(fd, &disks_info_ptr, &disks_info_count)) {
		ret = -1;
		goto exit;
	}

	for (i = 0 ; i < disks_info_count ; i++) {
		printf("%s\t%10s\n", disks_info_ptr[i].path,
			df_pretty_sizes(disks_info_ptr[i].size, mode));

		print_disk_chunks(fd, disks_info_ptr[i].devid,
				disks_info_ptr[i].size,
				info_ptr, info_count,
				mode);
		printf("\n");

	}


exit:

	if (disks_info_ptr)
		free(disks_info_ptr);
	if (info_ptr)
		free(info_ptr);

	return ret;
}

const char * const cmd_device_disk_usage_usage[] = {
	"btrfs device disk-usage [-b] <path> [<path>..]",
	"Show which chunks are in a device.",
	"",
	"-b\tSet byte as unit",
	NULL
};

int cmd_device_disk_usage(int argc, char **argv)
{

	int	flags = DF_HUMAN_UNIT;
	int	i, more_than_one = 0;

	optind = 1;
	while (1) {
		char	c = getopt(argc, argv, "b");

		if (c < 0)
			break;

		switch (c) {
		case 'b':
			flags &= ~DF_HUMAN_UNIT;
			break;
		default:
			usage(cmd_device_disk_usage_usage);
		}
	}

	if (check_argc_min(argc - optind, 1)) {
		usage(cmd_device_disk_usage_usage);
		return 21;
	}

	for (i = optind; i < argc ; i++) {
		int r, fd;
		DIR	*dirstream = NULL;
		if (more_than_one)
			printf("\n");

		fd = open_file_or_dir(argv[i], &dirstream);
		if (fd < 0) {
			fprintf(stderr, "ERROR: can't access to '%s'\n",
				argv[1]);
			return 12;
		}
		r = _cmd_device_disk_usage(fd, argv[i], flags);
		close_file_or_dir(fd, dirstream);

		if (r)
			return r;
		more_than_one = 1;

	}

	return 0;
}
