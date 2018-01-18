/*
 * Copyright (C) 2018 Facebook
 *
 * This file is part of libbtrfsutil.
 *
 * libbtrfsutil is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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
extern PyTypeObject QgroupInherit_type;

/*
 * Helpers for path arguments based on posixmodule.c in CPython.
 */
struct path_arg {
	bool allow_fd;
	char *path;
	int fd;
	Py_ssize_t length;
	PyObject *object;
	PyObject *cleanup;
};
int path_converter(PyObject *o, void *p);
void path_cleanup(struct path_arg *path);

void SetFromBtrfsUtilError(enum btrfs_util_error err);
void SetFromBtrfsUtilErrorWithPath(enum btrfs_util_error err,
				   struct path_arg *path);
void SetFromBtrfsUtilErrorWithPaths(enum btrfs_util_error err,
				    struct path_arg *path1,
				    struct path_arg *path2);

void add_module_constants(PyObject *m);

#endif /* BTRFSUTILPY_H */
