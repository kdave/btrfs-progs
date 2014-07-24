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
#include "string-table.h"
#include "cmds-fi-disk_usage.h"
#include "commands.h"

#include "version.h"

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
				free(*info_ptr);
				fprintf(stderr, "ERROR: not enough memory\n");
				return -ENOMEM;
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

static int load_chunk_info(int fd, struct chunk_info **info_ptr, int *info_count)
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
		if (ret == -EPERM)
			return ret;

		if (ret < 0) {
			fprintf(stderr,
				"ERROR: can't perform the search - %s\n",
				strerror(e));
			return ret;
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

			ret = add_info_to_list(info_ptr, info_count, item);
			if (ret) {
				*info_ptr = 0;
				return ret;
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

	sargs_orig = sargs = calloc(1, sizeof(struct btrfs_ioctl_space_args));
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
 * which compose the chunk, which could be different from the number of devices
 * if a disk is added later.
 */
static void get_raid56_used(int fd, struct chunk_info *chunks, int chunkcount,
		u64 *raid5_used, u64 *raid6_used)
{
	struct chunk_info *info_ptr = chunks;
	*raid5_used = 0;
	*raid6_used = 0;

	while (chunkcount-- > 0) {
		if (info_ptr->type & BTRFS_BLOCK_GROUP_RAID5)
			(*raid5_used) += info_ptr->size / (info_ptr->num_stripes - 1);
		if (info_ptr->type & BTRFS_BLOCK_GROUP_RAID6)
			(*raid6_used) += info_ptr->size / (info_ptr->num_stripes - 2);
		info_ptr++;
	}
}

static int print_filesystem_usage_overall(int fd, struct chunk_info *chunkinfo,
		int chunkcount, struct device_info *devinfo, int devcount,
		char *path, int mode)
{
	struct btrfs_ioctl_space_args *sargs = 0;
	int i;
	int ret = 0;
	int e, width;
	u64 total_disk;		/* filesystem size == sum of
				   device sizes */
	u64 total_chunks;	/* sum of chunks sizes on disk(s) */
	u64 total_used;		/* logical space used */
	u64 total_free;		/* logical space un-used */
	double K;
	u64 raid5_used, raid6_used;
	u64 global_reserve;
	u64 global_reserve_used;

	sargs = load_space_info(fd, path);
	if (!sargs) {
		ret = 1;
		goto exit;
	}

	total_disk = disk_size(path);
	e = errno;
	if (total_disk == 0) {
		fprintf(stderr,
			"ERROR: couldn't get space info on '%s' - %s\n",
			path, strerror(e));

		ret = 1;
		goto exit;
	}
	get_raid56_used(fd, chunkinfo, chunkcount, &raid5_used, &raid6_used);

	total_chunks = 0;
	total_used = 0;
	total_free = 0;
	global_reserve = 0;
	global_reserve_used = 0;

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

		if (flags & BTRFS_SPACE_INFO_GLOBAL_RSV) {
			global_reserve = sargs->spaces[i].total_bytes;
			global_reserve_used = sargs->spaces[i].used_bytes;
		}

		allocated = sargs->spaces[i].total_bytes * ratio;

		total_chunks += allocated;
		total_used += sargs->spaces[i].used_bytes;
		total_free += (sargs->spaces[i].total_bytes -
					sargs->spaces[i].used_bytes);

	}

	/* add the raid5/6 allocated space */
	total_chunks += raid5_used + raid6_used;

	K = ((double)total_used + (double)total_free) /	(double)total_chunks;

	if (mode == UNITS_HUMAN)
		width = 10;
	else
		width = 18;

	printf("Overall:\n");

	printf("    Device size:\t\t%*s\n", width,
		pretty_size_mode(total_disk, mode));
	printf("    Device allocated:\t\t%*s\n", width,
		pretty_size_mode(total_chunks, mode));
	printf("    Device unallocated:\t\t%*s\n", width,
		pretty_size_mode(total_disk - total_chunks, mode));
	printf("    Used:\t\t\t%*s\n", width,
		pretty_size_mode(total_used, mode));
	printf("    Free (Estimated):\t\t%*s\t(",
		width,
		pretty_size_mode((u64)(K * total_disk - total_used), mode));
	printf("Max: %s, ",
		pretty_size_mode(total_disk - total_chunks + total_free, mode));
	printf("min: %s)\n",
		pretty_size_mode((total_disk-total_chunks) / 2 + total_free, mode));
	printf("    Data to device ratio:\t%*.0f %%\n",
		width - 2, K * 100);
	printf("    Global reserve:\t\t%*s\t(used: %s)\n", width,
		pretty_size_mode(global_reserve, mode),
		pretty_size_mode(global_reserve_used, mode));

exit:

	if (sargs)
		free(sargs);

	return ret;
}

/*
 *  Helper to sort the device_info structure
 */
static int cmp_device_info(const void *a, const void *b)
{
	return strcmp(((struct device_info *)a)->path,
			((struct device_info *)b)->path);
}

/*
 *  This function loads the device_info structure and put them in an array
 */
static int load_device_info(int fd, struct device_info **device_info_ptr,
			   int *device_info_count)
{
	int ret, i, ndevs;
	struct btrfs_ioctl_fs_info_args fi_args;
	struct btrfs_ioctl_dev_info_args dev_info;
	struct device_info *info;

	*device_info_count = 0;
	*device_info_ptr = 0;

	ret = ioctl(fd, BTRFS_IOC_FS_INFO, &fi_args);
	if (ret == -EPERM)
		return ret;
	if (ret < 0) {
		fprintf(stderr, "ERROR: cannot get filesystem info\n");
		return ret;
	}

	info = calloc(fi_args.num_devices, sizeof(struct device_info));
	if (!info) {
		fprintf(stderr, "ERROR: not enough memory\n");
		return ret;
	}

	for (i = 0, ndevs = 0 ; i <= fi_args.max_id ; i++) {
		BUG_ON(ndevs >= fi_args.num_devices);
		memset(&dev_info, 0, sizeof(dev_info));
		ret = get_device_info(fd, i, &dev_info);

		if (ret == -ENODEV)
			continue;
		if (ret) {
			fprintf(stderr,
			    "ERROR: cannot get info about device devid=%d\n",
			    i);
			free(info);
			return ret;
		}

		info[ndevs].devid = dev_info.devid;
		strcpy(info[ndevs].path, (char *)dev_info.path);
		info[ndevs].device_size = get_partition_size((char *)dev_info.path);
		info[ndevs].size = dev_info.total_bytes;
		++ndevs;
	}

	BUG_ON(ndevs != fi_args.num_devices);
	qsort(info, fi_args.num_devices,
		sizeof(struct device_info), cmp_device_info);

	*device_info_count = fi_args.num_devices;
	*device_info_ptr = info;

	return 0;
}

int load_chunk_and_device_info(int fd, struct chunk_info **chunkinfo,
		int *chunkcount, struct device_info **devinfo, int *devcount)
{
	int ret;

	ret = load_chunk_info(fd, chunkinfo, chunkcount);
	if (ret == -EPERM) {
		fprintf(stderr,
			"WARNING: can't read detailed chunk info, RAID5/6 numbers will be incorrect, run as root\n");
	} else if (ret) {
		return ret;
	}

	ret = load_device_info(fd, devinfo, devcount);
	if (ret == -EPERM) {
		fprintf(stderr,
			"WARNING: can't get filesystem info from ioctl(FS_INFO), run as root\n");
		ret = 0;
	}

	return ret;
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
 *  This function print the results of the command "btrfs fi usage"
 *  in tabular format
 */
static void _cmd_filesystem_usage_tabular(int mode,
					struct btrfs_ioctl_space_args *sargs,
					struct chunk_info *chunks_info_ptr,
					int chunks_info_count,
					struct device_info *device_info_ptr,
					int device_info_count)
{
	int i;
	u64 total_unused = 0;
	struct string_table *matrix = 0;
	int  ncols, nrows;

	ncols = sargs->total_spaces + 2;
	nrows = 2 + 1 + device_info_count + 1 + 2;

	matrix = table_create(ncols, nrows);
	if (!matrix) {
		fprintf(stderr, "ERROR: not enough memory\n");
		return;
	}

	/* header */
	for (i = 0; i < sargs->total_spaces; i++) {
		const char *description;
		u64 flags = sargs->spaces[i].flags;

		if (flags & BTRFS_SPACE_INFO_GLOBAL_RSV)
			continue;

		description = btrfs_group_type_str(flags);

		table_printf(matrix, 1+i, 0, "<%s", description);
	}

	for (i = 0; i < sargs->total_spaces; i++) {
		const char *r_mode;

		u64 flags = sargs->spaces[i].flags;
		r_mode = btrfs_group_profile_str(flags);

		table_printf(matrix, 1+i, 1, "<%s", r_mode);
	}

	table_printf(matrix, 1+sargs->total_spaces, 1, "<Unallocated");

	/* body */
	for (i = 0; i < device_info_count; i++) {
		int k, col;
		char *p;

		u64  total_allocated = 0, unused;

		p = strrchr(device_info_ptr[i].path, '/');
		if (!p)
			p = device_info_ptr[i].path;
		else
			p++;

		table_printf(matrix, 0, i + 3, "<%s", device_info_ptr[i].path);

		for (col = 1, k = 0 ; k < sargs->total_spaces ; k++)  {
			u64	flags = sargs->spaces[k].flags;
			u64 devid = device_info_ptr[i].devid;
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
					">%s", pretty_size_mode(size, mode));
			else
				table_printf(matrix, col, i+3, ">-");

			total_allocated += size;
			col++;
		}

		unused = get_partition_size(device_info_ptr[i].path)
				- total_allocated;

		table_printf(matrix, sargs->total_spaces + 1, i + 3,
			       ">%s", pretty_size_mode(unused, mode));
		total_unused += unused;

	}

	for (i = 0; i <= sargs->total_spaces; i++)
		table_printf(matrix, i + 1, device_info_count + 3, "=");

	/* footer */
	table_printf(matrix, 0, device_info_count + 4, "<Total");
	for (i = 0; i < sargs->total_spaces; i++)
		table_printf(matrix, 1 + i, device_info_count + 4, ">%s",
			pretty_size_mode(sargs->spaces[i].total_bytes, mode));

	table_printf(matrix, sargs->total_spaces + 1, device_info_count + 4,
			">%s", pretty_size_mode(total_unused, mode));

	table_printf(matrix, 0, device_info_count + 5, "<Used");
	for (i = 0; i < sargs->total_spaces; i++)
		table_printf(matrix, 1 + i, device_info_count+5, ">%s",
			pretty_size_mode(sargs->spaces[i].used_bytes, mode));

	table_dump(matrix);
	table_free(matrix);
}

/*
 *  This function prints the unused space per every disk
 */
static void print_unused(struct chunk_info *info_ptr,
			  int info_count,
			  struct device_info *device_info_ptr,
			  int device_info_count,
			  int mode)
{
	int i;
	for (i = 0; i < device_info_count; i++) {
		int	j;
		u64	total = 0;

		for (j = 0; j < info_count; j++)
			if (info_ptr[j].devid == device_info_ptr[i].devid)
				total += calc_chunk_size(info_ptr+j);

		printf("   %s\t%10s\n",
			device_info_ptr[i].path,
			pretty_size_mode(device_info_ptr[i].size - total, mode));
	}
}

/*
 *  This function prints the allocated chunk per every disk
 */
static void print_chunk_device(u64 chunk_type,
				struct chunk_info *chunks_info_ptr,
				int chunks_info_count,
				struct device_info *device_info_ptr,
				int device_info_count,
				int mode)
{
	int i;

	for (i = 0; i < device_info_count; i++) {
		int	j;
		u64	total = 0;

		for (j = 0; j < chunks_info_count; j++) {

			if (chunks_info_ptr[j].type != chunk_type)
				continue;
			if (chunks_info_ptr[j].devid != device_info_ptr[i].devid)
				continue;

			total += calc_chunk_size(&(chunks_info_ptr[j]));
			//total += chunks_info_ptr[j].size;
		}

		if (total > 0)
			printf("   %s\t%10s\n",
				device_info_ptr[i].path,
				pretty_size_mode(total, mode));
	}
}

/*
 *  This function print the results of the command "btrfs fi usage"
 *  in linear format
 */
static void _cmd_filesystem_usage_linear(int mode,
					struct btrfs_ioctl_space_args *sargs,
					struct chunk_info *info_ptr,
					int info_count,
					struct device_info *device_info_ptr,
					int device_info_count)
{
	int i;

	for (i = 0; i < sargs->total_spaces; i++) {
		const char *description;
		const char *r_mode;
		u64 flags = sargs->spaces[i].flags;

		if (flags & BTRFS_SPACE_INFO_GLOBAL_RSV)
			continue;

		description = btrfs_group_type_str(flags);
		r_mode = btrfs_group_profile_str(flags);

		printf("%s,%s: Size:%s, ",
			description,
			r_mode,
			pretty_size_mode(sargs->spaces[i].total_bytes,
			    mode));
		printf("Used:%s\n",
			pretty_size_mode(sargs->spaces[i].used_bytes, mode));
		print_chunk_device(flags, info_ptr, info_count,
				device_info_ptr, device_info_count, mode);
		printf("\n");
	}

	printf("Unallocated:\n");
	print_unused(info_ptr, info_count, device_info_ptr, device_info_count,
			mode);
}

static int print_filesystem_usage_by_chunk(int fd,
		struct chunk_info *chunkinfo, int chunkcount,
		struct device_info *devinfo, int devcount,
		char *path, int mode, int tabular)
{
	struct btrfs_ioctl_space_args *sargs;
	int ret = 0;

	if (!chunkinfo)
		return 0;

	sargs = load_space_info(fd, path);
	if (!sargs) {
		ret = 1;
		goto out;
	}

	if (tabular)
		_cmd_filesystem_usage_tabular(mode, sargs, chunkinfo,
				chunkcount, devinfo, devcount);
	else
		_cmd_filesystem_usage_linear(mode, sargs, chunkinfo,
				chunkcount, devinfo, devcount);

	free(sargs);
out:
	return ret;
}

const char * const cmd_filesystem_usage_usage[] = {
	"btrfs filesystem usage [-b][-t] <path> [<path>..]",
	"Show in which disk the chunks are allocated.",
	"",
	"-b\tSet byte as unit",
	"-t\tShow data in tabular format",
	NULL
};

int cmd_filesystem_usage(int argc, char **argv)
{
	int mode = UNITS_HUMAN;
	int ret = 0;
	int	i, more_than_one = 0;
	int	tabular = 0;

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "bt");

		if (c < 0)
			break;

		switch (c) {
		case 'b':
			mode = UNITS_RAW;
			break;
		case 't':
			tabular = 1;
			break;
		default:
			usage(cmd_filesystem_usage_usage);
		}
	}

	if (check_argc_min(argc - optind, 1))
		usage(cmd_filesystem_usage_usage);

	for (i = optind; i < argc; i++) {
		int fd;
		DIR *dirstream = NULL;
		struct chunk_info *chunkinfo = NULL;
		struct device_info *devinfo = NULL;
		int chunkcount = 0;
		int devcount = 0;

		fd = open_file_or_dir(argv[i], &dirstream);
		if (fd < 0) {
			fprintf(stderr, "ERROR: can't access '%s'\n",
				argv[1]);
			ret = 1;
			goto out;
		}
		if (more_than_one)
			printf("\n");

		ret = load_chunk_and_device_info(fd, &chunkinfo, &chunkcount,
				&devinfo, &devcount);
		if (ret)
			goto cleanup;

		ret = print_filesystem_usage_overall(fd, chunkinfo, chunkcount,
				devinfo, devcount, argv[i], mode);
		if (ret)
			goto cleanup;
		printf("\n");
		ret = print_filesystem_usage_by_chunk(fd, chunkinfo, chunkcount,
				devinfo, devcount, argv[i], mode, tabular);
cleanup:
		close_file_or_dir(fd, dirstream);
		free(chunkinfo);
		free(devinfo);

		if (ret)
			goto out;
		more_than_one = 1;
	}

out:
	return !!ret;
}

void print_device_chunks(int fd, struct device_info *devinfo,
		struct chunk_info *chunks_info_ptr,
		int chunks_info_count, int mode)
{
	int i;
	u64 allocated = 0;

	for (i = 0 ; i < chunks_info_count ; i++) {
		const char *description;
		const char *r_mode;
		u64 flags;
		u64 size;

		if (chunks_info_ptr[i].devid != devinfo->devid)
			continue;

		flags = chunks_info_ptr[i].type;

		description = btrfs_group_type_str(flags);
		r_mode = btrfs_group_profile_str(flags);
		size = calc_chunk_size(chunks_info_ptr+i);
		printf("   %s,%s:%*s%10s\n",
			description,
			r_mode,
			(int)(20 - strlen(description) - strlen(r_mode)), "",
			pretty_size_mode(size, mode));

		allocated += size;

	}
	printf("   Unallocated: %*s%10s\n",
		(int)(20 - strlen("Unallocated")), "",
		pretty_size_mode(devinfo->size - allocated, mode));
}

void print_device_sizes(int fd, struct device_info *devinfo, int mode)
{
	printf("   Device size: %*s%10s\n",
		(int)(20 - strlen("Device size")), "",
		pretty_size_mode(devinfo->device_size, mode));
#if 0
	/*
	 * The term has not seen an agreement and we don't want to change it
	 * once it's in non-development branches or even released.
	 */
	printf("   FS occupied: %*s%10s\n",
		(int)(20 - strlen("FS occupied")), "",
		pretty_size_mode(devinfo->size, mode));
#endif
}
