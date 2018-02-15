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

#include "btrfsutilpy.h"

PyObject *is_subvolume(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", NULL};
	struct path_arg path = {.allow_fd = true};
	enum btrfs_util_error err;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:is_subvolume",
					 keywords, &path_converter, &path))
		return NULL;

	if (path.path)
		err = btrfs_util_is_subvolume(path.path);
	else
		err = btrfs_util_is_subvolume_fd(path.fd);
	if (err == BTRFS_UTIL_OK) {
		path_cleanup(&path);
		Py_RETURN_TRUE;
	} else if (err == BTRFS_UTIL_ERROR_NOT_BTRFS ||
		   err == BTRFS_UTIL_ERROR_NOT_SUBVOLUME) {
		path_cleanup(&path);
		Py_RETURN_FALSE;
	} else {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}
}

PyObject *subvolume_id(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", NULL};
	struct path_arg path = {.allow_fd = true};
	enum btrfs_util_error err;
	uint64_t id;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:subvolume_id",
					 keywords, &path_converter, &path))
		return NULL;

	if (path.path)
		err = btrfs_util_subvolume_id(path.path, &id);
	else
		err = btrfs_util_subvolume_id_fd(path.fd, &id);
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);
	return PyLong_FromUnsignedLongLong(id);
}

PyObject *subvolume_path(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", "id", NULL};
	struct path_arg path = {.allow_fd = true};
	enum btrfs_util_error err;
	uint64_t id = 0;
	char *subvol_path;
	PyObject *ret;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&|K:subvolume_path",
					 keywords, &path_converter, &path, &id))
		return NULL;

	if (path.path)
		err = btrfs_util_subvolume_path(path.path, id, &subvol_path);
	else
		err = btrfs_util_subvolume_path_fd(path.fd, id, &subvol_path);
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);

	ret = PyUnicode_DecodeFSDefault(subvol_path);
	free(subvol_path);
	return ret;
}

PyObject *create_subvolume(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", "async", "qgroup_inherit", NULL};
	struct path_arg path = {.allow_fd = false};
	enum btrfs_util_error err;
	int async = 0;
	QgroupInherit *inherit = NULL;
	uint64_t transid;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&|pO!:create_subvolume",
					 keywords, &path_converter, &path,
					 &async, &QgroupInherit_type, &inherit))
		return NULL;

	err = btrfs_util_create_subvolume(path.path, 0, async ? &transid : NULL,
					  inherit ? inherit->inherit : NULL);
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);
	if (async)
		return PyLong_FromUnsignedLongLong(transid);
	else
		Py_RETURN_NONE;
}
