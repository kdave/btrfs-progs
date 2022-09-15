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

#ifndef __PARSE_UTILS_H__
#define __PARSE_UTILS_H__

#include "kerncompat.h"
#include "kernel-shared/ctree.h"

u64 parse_size_from_string(const char *s);
enum btrfs_csum_type parse_csum_type(const char *s);

int parse_u64(const char *str, u64 *result);
int parse_range_u32(const char *range, u32 *start, u32 *end);
int parse_range(const char *range, u64 *start, u64 *end);
int parse_range_strict(const char *range, u64 *start, u64 *end);
int parse_bg_profile(const char *profile, u64 *flags);
int parse_compress_type(const char *type);
int parse_qgroupid(const char *str, u64 *qgroupid);
u64 parse_qgroupid_or_path(const char *p);
int fls64(u64 x);

#endif
