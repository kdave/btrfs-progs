/*
 * Copyright (C) 2018 Facebook
 *
 * This file is part of libbtrfsutil.
 *
 * libbtrfsutil is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * libbtrfsutil is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libbtrfsutil.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "btrfsutil_internal.h"

PUBLIC enum btrfs_util_error btrfs_util_create_qgroup_inherit(int flags,
							      struct btrfs_util_qgroup_inherit **ret)
{
	struct btrfs_qgroup_inherit *inherit;

	if (flags) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}

	inherit = calloc(1, sizeof(*inherit));
	if (!inherit)
		return BTRFS_UTIL_ERROR_NO_MEMORY;

	/*
	 * struct btrfs_util_qgroup_inherit is a lie; it's actually struct
	 * btrfs_qgroup_inherit, but we abstract it away so that users don't
	 * need to depend on the Btrfs UAPI headers.
	 */
	*ret = (struct btrfs_util_qgroup_inherit *)inherit;

	return BTRFS_UTIL_OK;
}

PUBLIC void btrfs_util_destroy_qgroup_inherit(struct btrfs_util_qgroup_inherit *inherit)
{
	free(inherit);
}

PUBLIC enum btrfs_util_error btrfs_util_qgroup_inherit_add_group(struct btrfs_util_qgroup_inherit **inherit,
								 uint64_t qgroupid)
{
	struct btrfs_qgroup_inherit *tmp = (struct btrfs_qgroup_inherit *)*inherit;

	tmp = realloc(tmp, sizeof(*tmp) +
		      (tmp->num_qgroups + 1) * sizeof(tmp->qgroups[0]));
	if (!tmp)
		return BTRFS_UTIL_ERROR_NO_MEMORY;

	tmp->qgroups[tmp->num_qgroups++] = qgroupid;

	*inherit = (struct btrfs_util_qgroup_inherit *)tmp;

	return BTRFS_UTIL_OK;
}

PUBLIC void btrfs_util_qgroup_inherit_get_groups(const struct btrfs_util_qgroup_inherit *inherit,
						 const uint64_t **groups,
						 size_t *n)
{
	struct btrfs_qgroup_inherit *tmp = (struct btrfs_qgroup_inherit *)inherit;

	/* Need to cast because __u64 != uint64_t. */
	*groups = (const uint64_t *)&tmp->qgroups[0];
	*n = tmp->num_qgroups;
}
