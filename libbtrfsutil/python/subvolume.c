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

static PyObject *subvolume_info_to_object(const struct btrfs_util_subvolume_info *subvol)
{
	PyObject *ret, *tmp;

	ret = PyStructSequence_New(&SubvolumeInfo_type);
	if (ret == NULL)
		return NULL;

#define SET_UINT64(i, field)					\
	tmp = PyLong_FromUnsignedLongLong(subvol->field);	\
	if (tmp == NULL) {					\
		Py_DECREF(ret);					\
		return ret;					\
	}							\
	PyStructSequence_SET_ITEM(ret, i, tmp);

#define SET_UUID(i, field)						\
	tmp = PyBytes_FromStringAndSize((char *)subvol->field, 16);	\
	if (tmp == NULL) {						\
		Py_DECREF(ret);						\
		return ret;						\
	}								\
	PyStructSequence_SET_ITEM(ret, i, tmp);

#define SET_TIME(i, field)						\
	tmp = PyFloat_FromDouble(subvol->field.tv_sec +			\
				 subvol->field.tv_nsec / 1000000000);	\
	if (tmp == NULL) {						\
		Py_DECREF(ret);						\
		return ret;						\
	}								\
	PyStructSequence_SET_ITEM(ret, i, tmp);

	SET_UINT64(0, id);
	SET_UINT64(1, parent_id);
	SET_UINT64(2, dir_id);
	SET_UINT64(3, flags);
	SET_UUID(4, uuid);
	SET_UUID(5, parent_uuid);
	SET_UUID(6, received_uuid);
	SET_UINT64(7, generation);
	SET_UINT64(8, ctransid);
	SET_UINT64(9, otransid);
	SET_UINT64(10, stransid);
	SET_UINT64(11, rtransid);
	SET_TIME(12, ctime);
	SET_TIME(13, otime);
	SET_TIME(14, stime);
	SET_TIME(15, rtime);

#undef SET_TIME
#undef SET_UUID
#undef SET_UINT64

	return ret;
}

PyObject *subvolume_info(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", "id", NULL};
	struct path_arg path = {.allow_fd = true};
	struct btrfs_util_subvolume_info subvol;
	enum btrfs_util_error err;
	uint64_t id = 0;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&|K:subvolume_info",
					 keywords, &path_converter, &path, &id))
		return NULL;

	if (path.path)
		err = btrfs_util_subvolume_info(path.path, id, &subvol);
	else
		err = btrfs_util_subvolume_info_fd(path.fd, id, &subvol);
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);

	return subvolume_info_to_object(&subvol);
}

static PyStructSequence_Field SubvolumeInfo_fields[] = {
	{"id", "int ID of this subvolume"},
	{"parent_id", "int ID of the subvolume containing this subvolume"},
	{"dir_id", "int inode number of the directory containing this subvolume"},
	{"flags", "int root item flags"},
	{"uuid", "bytes UUID of this subvolume"},
	{"parent_uuid", "bytes UUID of the subvolume this is a snapshot of"},
	{"received_uuid", "bytes UUID of the subvolume this was received from"},
	{"generation", "int transaction ID of the subvolume root"},
	{"ctransid", "int transaction ID when an inode was last changed"},
	{"otransid", "int transaction ID when this subvolume was created"},
	{"stransid", "int transaction ID of the sent subvolume this subvolume was received from"},
	{"rtransid", "int transaction ID when this subvolume was received"},
	{"ctime", "float time when an inode was last changed"},
	{"otime", "float time when this subvolume was created"},
	{"stime", "float time, usually zero"},
	{"rtime", "float time when this subvolume was received"},
	{},
};

PyStructSequence_Desc SubvolumeInfo_desc = {
	"btrfsutil.SubvolumeInfo",
	"Information about a Btrfs subvolume.",
	SubvolumeInfo_fields,
	14,
};

PyTypeObject SubvolumeInfo_type;

PyObject *get_subvolume_read_only(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", NULL};
	struct path_arg path = {.allow_fd = true};
	enum btrfs_util_error err;
	bool read_only;

	if (!PyArg_ParseTupleAndKeywords(args, kwds,
					 "O&:get_subvolume_read_only",
					 keywords, &path_converter, &path))
		return NULL;

	if (path.path) {
		err = btrfs_util_get_subvolume_read_only(path.path, &read_only);
	} else {
		err = btrfs_util_get_subvolume_read_only_fd(path.fd,
							    &read_only);
	}
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);
	return PyBool_FromLong(read_only);
}

PyObject *set_subvolume_read_only(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", "read_only", NULL};
	struct path_arg path = {.allow_fd = true};
	enum btrfs_util_error err;
	int read_only = 1;

	if (!PyArg_ParseTupleAndKeywords(args, kwds,
					 "O&|p:set_subvolume_read_only",
					 keywords, &path_converter, &path,
					 &read_only))
		return NULL;

	if (path.path)
		err = btrfs_util_set_subvolume_read_only(path.path, read_only);
	else
		err = btrfs_util_set_subvolume_read_only_fd(path.fd, read_only);
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);
	Py_RETURN_NONE;
}

PyObject *get_default_subvolume(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", NULL};
	struct path_arg path = {.allow_fd = true};
	enum btrfs_util_error err;
	uint64_t id;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:get_default_subvolume",
					 keywords, &path_converter, &path))
		return NULL;

	if (path.path)
		err = btrfs_util_get_default_subvolume(path.path, &id);
	else
		err = btrfs_util_get_default_subvolume_fd(path.fd, &id);
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);
	return PyLong_FromUnsignedLongLong(id);
}

PyObject *set_default_subvolume(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", "id", NULL};
	struct path_arg path = {.allow_fd = true};
	enum btrfs_util_error err;
	uint64_t id = 0;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&|K:set_default_subvolume",
					 keywords, &path_converter, &path, &id))
		return NULL;

	if (path.path)
		err = btrfs_util_set_default_subvolume(path.path, id);
	else
		err = btrfs_util_set_default_subvolume_fd(path.fd, id);
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);
	Py_RETURN_NONE;
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
