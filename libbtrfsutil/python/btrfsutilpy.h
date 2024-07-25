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

#ifndef BTRFSUTILPY_H
#define BTRFSUTILPY_H

#define PY_SSIZE_T_CLEAN

#include <stdbool.h>
#include <stddef.h>
#include <Python.h>
#include "structmember.h"

#include <btrfsutil.h>

typedef struct {
	PyObject_HEAD
	struct btrfs_util_qgroup_inherit *inherit;
} QgroupInherit;

extern PyTypeObject BtrfsUtilError_type;
extern PyStructSequence_Desc SubvolumeInfo_desc;
extern PyTypeObject SubvolumeInfo_type;
extern PyTypeObject SubvolumeIterator_type;
extern PyTypeObject QgroupInherit_type;

struct path_arg {
	bool allow_fd;
	int fd;
	char *path;
	Py_ssize_t length;
	PyObject *object;
	PyObject *bytes;
};
int path_converter(PyObject *o, void *p);
void path_cleanup(struct path_arg *path);

PyObject *list_from_uint64_array(const uint64_t *arr, size_t n);

void SetFromBtrfsUtilError(enum btrfs_util_error err);
void SetFromBtrfsUtilErrorWithPath(enum btrfs_util_error err,
				   struct path_arg *path);
void SetFromBtrfsUtilErrorWithPaths(enum btrfs_util_error err,
				    struct path_arg *path1,
				    struct path_arg *path2);

PyObject *filesystem_sync(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *start_sync(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *wait_sync(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *is_subvolume(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *subvolume_id(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *subvolume_path(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *subvolume_info(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *get_subvolume_read_only(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *set_subvolume_read_only(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *get_default_subvolume(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *set_default_subvolume(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *create_subvolume(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *create_snapshot(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *delete_subvolume(PyObject *self, PyObject *args, PyObject *kwds);
PyObject *deleted_subvolumes(PyObject *self, PyObject *args, PyObject *kwds);

void add_module_constants(PyObject *m);

#endif /* BTRFSUTILPY_H */
