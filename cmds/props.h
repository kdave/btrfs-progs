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

#ifndef __BTRFS_PROPS_H__
#define __BTRFS_PROPS_H__

enum prop_object_type {
	prop_object_dev		= (1 << 0),
	prop_object_root	= (1 << 1),
	prop_object_subvol	= (1 << 2),
	prop_object_inode	= (1 << 3),
	__prop_object_max,
};

typedef int (*prop_handler_t)(enum prop_object_type type,
			      const char *object,
			      const char *name,
			      const char *value,
			      bool force);

struct prop_handler {
	const char *name;
	const char *desc;
	int read_only;
	int types;
	prop_handler_t handler;
};

extern const struct prop_handler prop_handlers[];

#endif
