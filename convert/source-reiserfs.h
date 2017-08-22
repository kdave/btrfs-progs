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

#ifndef __BTRFS_CONVERT_SOURCE_REISERFS_H__
#define __BTRFS_CONVERT_SOURCE_REISERFS_H__

#if BTRFSCONVERT_REISERFS

#include "kerncompat.h"
#include <reiserfs/misc.h>
#include <reiserfs/io.h>
#include <reiserfs/reiserfs_lib.h>
#include <reiserfs/reiserfs_fs.h>
#include <linux/kdev_t.h>
#include "convert/source-fs.h"

#define REISERFS_ACL_VERSION	0x0001

static inline dev_t new_decode_dev(u32 dev)
{
	unsigned major = (dev & 0xfff00) >> 8;
	unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);

	return MKDEV(major, minor);
}

#endif	/* BTRFSCONVERT_REISERFS */

#endif
