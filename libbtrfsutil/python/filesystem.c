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

#include "btrfsutilpy.h"

PyObject *filesystem_sync(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", NULL};
	struct path_arg path = {.allow_fd = true};
	enum btrfs_util_error err;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:sync", keywords,
					 &path_converter, &path))
		return NULL;

	if (path.path)
		err = btrfs_util_sync(path.path);
	else
		err = btrfs_util_sync_fd(path.fd);
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);
	Py_RETURN_NONE;
}

PyObject *start_sync(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", NULL};
	struct path_arg path = {.allow_fd = true};
	uint64_t transid;
	enum btrfs_util_error err;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:start_sync", keywords,
					 &path_converter, &path))
		return NULL;

	if (path.path)
		err = btrfs_util_start_sync(path.path, &transid);
	else
		err = btrfs_util_start_sync_fd(path.fd, &transid);
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);
	return PyLong_FromUnsignedLongLong(transid);
}

PyObject *wait_sync(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", "transid", NULL};
	struct path_arg path = {.allow_fd = true};
	unsigned long long transid = 0;
	enum btrfs_util_error err;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&|K:wait_sync", keywords,
					 &path_converter, &path, &transid))
		return NULL;

	if (path.path)
		err = btrfs_util_wait_sync(path.path, transid);
	else
		err = btrfs_util_wait_sync_fd(path.fd, transid);
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);
	Py_RETURN_NONE;
}
