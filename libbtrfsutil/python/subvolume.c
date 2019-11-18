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
	static char *keywords[] = {"path", "async_", "qgroup_inherit", NULL};
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

PyObject *create_snapshot(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {
		"source", "path", "recursive", "read_only", "async_",
		"qgroup_inherit", NULL,
	};
	struct path_arg src = {.allow_fd = true}, dst = {.allow_fd = false};
	enum btrfs_util_error err;
	int recursive = 0, read_only = 0, async = 0;
	int flags = 0;
	QgroupInherit *inherit = NULL;
	uint64_t transid;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&O&|pppO!:create_snapshot",
					 keywords, &path_converter, &src,
					 &path_converter, &dst, &recursive,
					 &read_only, &async,
					 &QgroupInherit_type, &inherit))
		return NULL;

	if (recursive)
		flags |= BTRFS_UTIL_CREATE_SNAPSHOT_RECURSIVE;
	if (read_only)
		flags |= BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY;

	if (src.path) {
		err = btrfs_util_create_snapshot(src.path, dst.path, flags,
						 async ? &transid : NULL,
						 inherit ? inherit->inherit : NULL);
	} else {
		err = btrfs_util_create_snapshot_fd(src.fd, dst.path, flags,
						    async ? &transid : NULL,
						    inherit ? inherit->inherit : NULL);
	}
	if (err) {
		SetFromBtrfsUtilErrorWithPaths(err, &src, &dst);
		path_cleanup(&src);
		path_cleanup(&dst);
		return NULL;
	}

	path_cleanup(&src);
	path_cleanup(&dst);
	if (async)
		return PyLong_FromUnsignedLongLong(transid);
	else
		Py_RETURN_NONE;
}

PyObject *delete_subvolume(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", "recursive", NULL};
	struct path_arg path = {.allow_fd = false};
	enum btrfs_util_error err;
	int recursive = 0;
	int flags = 0;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&|p:delete_subvolume",
					 keywords, &path_converter, &path,
					 &recursive))
		return NULL;

	if (recursive)
		flags |= BTRFS_UTIL_DELETE_SUBVOLUME_RECURSIVE;

	err = btrfs_util_delete_subvolume(path.path, flags);
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);
	Py_RETURN_NONE;
}

PyObject *deleted_subvolumes(PyObject *self, PyObject *args, PyObject *kwds)
{
	static char *keywords[] = {"path", NULL};
	struct path_arg path = {.allow_fd = true};
	PyObject *ret;
	uint64_t *ids;
	size_t n;
	enum btrfs_util_error err;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&:deleted_subvolumes",
					 keywords, &path_converter, &path))
		return NULL;

	if (path.path)
		err = btrfs_util_deleted_subvolumes(path.path, &ids, &n);
	else
		err = btrfs_util_deleted_subvolumes_fd(path.fd, &ids, &n);
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return NULL;
	}

	path_cleanup(&path);

	ret = list_from_uint64_array(ids, n);
	free(ids);
	return ret;
}

typedef struct {
	PyObject_HEAD
	struct btrfs_util_subvolume_iterator *iter;
	bool info;
} SubvolumeIterator;

static void SubvolumeIterator_dealloc(SubvolumeIterator *self)
{
	btrfs_util_destroy_subvolume_iterator(self->iter);
	Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *SubvolumeIterator_next(SubvolumeIterator *self)
{
	enum btrfs_util_error err;
	PyObject *ret, *tmp;
	char *path;

	if (!self->iter) {
		PyErr_SetString(PyExc_ValueError,
				"operation on closed iterator");
		return NULL;
	}

	if (self->info) {
		struct btrfs_util_subvolume_info subvol;

		err = btrfs_util_subvolume_iterator_next_info(self->iter, &path,
							      &subvol);
		if (err == BTRFS_UTIL_ERROR_STOP_ITERATION) {
			PyErr_SetNone(PyExc_StopIteration);
			return NULL;
		} else if (err) {
			SetFromBtrfsUtilError(err);
			return NULL;
		}

		tmp = subvolume_info_to_object(&subvol);
	} else {
		uint64_t id;

		err = btrfs_util_subvolume_iterator_next(self->iter, &path, &id);
		if (err == BTRFS_UTIL_ERROR_STOP_ITERATION) {
			PyErr_SetNone(PyExc_StopIteration);
			return NULL;
		} else if (err) {
			SetFromBtrfsUtilError(err);
			return NULL;
		}

		tmp = PyLong_FromUnsignedLongLong(id);

	}
	if (tmp) {
		ret = Py_BuildValue("O&O", PyUnicode_DecodeFSDefault, path,
				    tmp);
		Py_DECREF(tmp);
		free(path);
	} else {
		ret = NULL;
	}
	return ret;
}

static int SubvolumeIterator_init(SubvolumeIterator *self, PyObject *args,
				  PyObject *kwds)
{
	static char *keywords[] = {"path", "top", "info", "post_order", NULL};
	struct path_arg path = {.allow_fd = true};
	enum btrfs_util_error err;
	unsigned long long top = 0;
	int info = 0;
	int post_order = 0;
	int flags = 0;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&|Kpp:SubvolumeIterator",
					 keywords, &path_converter, &path, &top,
					 &info, &post_order))
		return -1;

	if (post_order)
		flags |= BTRFS_UTIL_SUBVOLUME_ITERATOR_POST_ORDER;

	if (path.path) {
		err = btrfs_util_create_subvolume_iterator(path.path, top,
							   flags, &self->iter);
	} else {
		err = btrfs_util_create_subvolume_iterator_fd(path.fd, top,
							      flags,
							      &self->iter);
	}
	if (err) {
		SetFromBtrfsUtilErrorWithPath(err, &path);
		path_cleanup(&path);
		return -1;
	}

	self->info = info;

	return 0;
}

static PyObject *SubvolumeIterator_close(SubvolumeIterator *self)
{
	if (self->iter) {
		btrfs_util_destroy_subvolume_iterator(self->iter);
		self->iter = NULL;
	}
	Py_RETURN_NONE;
}

static PyObject *SubvolumeIterator_fileno(SubvolumeIterator *self)
{
	if (!self->iter) {
		PyErr_SetString(PyExc_ValueError,
				"operation on closed iterator");
		return NULL;
	}
	return PyLong_FromLong(btrfs_util_subvolume_iterator_fd(self->iter));
}

static PyObject *SubvolumeIterator_enter(SubvolumeIterator *self)
{
	Py_INCREF((PyObject *)self);
	return (PyObject *)self;
}

static PyObject *SubvolumeIterator_exit(SubvolumeIterator *self, PyObject *args,
				       PyObject *kwds)
{
	static char *keywords[] = {"exc_type", "exc_value", "traceback", NULL};
	PyObject *exc_type, *exc_value, *traceback;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO:__exit__", keywords,
					 &exc_type, &exc_value, &traceback))
		return NULL;

	return SubvolumeIterator_close(self);
}

#define SubvolumeIterator_DOC	\
	 "SubvolumeIterator(path, top=0, info=False, post_order=False) -> new subvolume iterator\n\n"	\
	 "Create a new iterator that produces tuples of (path, ID) representing\n"	\
	 "subvolumes on a filesystem.\n\n"						\
	 "Arguments:\n"									\
	 "path -- string, bytes, path-like object, or open file descriptor in\n"	\
	 "filesystem to list\n"								\
	 "top -- if not zero, instead of only listing subvolumes beneath the\n"		\
	 "given path, list subvolumes beneath the subvolume with this ID; passing\n"	\
	 "BTRFS_FS_TREE_OBJECTID (5) here lists all subvolumes. The subvolumes\n"	\
	 "are listed relative to the subvolume with this ID.\n"				\
	 "info -- bool indicating the iterator should yield SubvolumeInfo instead of\n"	\
	 "the subvolume ID\n"								\
	 "post_order -- bool indicating whether to yield parent subvolumes before\n"	\
	 "child subvolumes (e.g., 'foo/bar' before 'foo')"

static PyMethodDef SubvolumeIterator_methods[] = {
	{"close", (PyCFunction)SubvolumeIterator_close,
	 METH_NOARGS,
	 "close()\n\n"
	 "Close this iterator."},
	{"fileno", (PyCFunction)SubvolumeIterator_fileno,
	 METH_NOARGS,
	 "fileno() -> int\n\n"
	 "Get the file descriptor associated with this iterator."},
	{"__enter__", (PyCFunction)SubvolumeIterator_enter,
	 METH_NOARGS, ""},
	{"__exit__", (PyCFunction)SubvolumeIterator_exit,
	 METH_VARARGS | METH_KEYWORDS, ""},
	{},
};

PyTypeObject SubvolumeIterator_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name		= "btrfsutil.SubvolumeIterator",
	.tp_basicsize		= sizeof(SubvolumeIterator),
	.tp_dealloc		= (destructor)SubvolumeIterator_dealloc,
	.tp_flags		= Py_TPFLAGS_DEFAULT,
	.tp_doc			= SubvolumeIterator_DOC,
	.tp_iter		= PyObject_SelfIter,
	.tp_iternext		= (iternextfunc)SubvolumeIterator_next,
	.tp_methods		= SubvolumeIterator_methods,
	.tp_init		= (initproc)SubvolumeIterator_init,
};
