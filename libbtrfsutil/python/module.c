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

#if 0
static int fd_converter(PyObject *o, void *p)
{
	int *fd = p;
	long tmp;
	int overflow;

	tmp = PyLong_AsLongAndOverflow(o, &overflow);
	if (tmp == -1 && PyErr_Occurred())
		return 0;
	if (overflow > 0 || tmp > INT_MAX) {
		PyErr_SetString(PyExc_OverflowError,
				"fd is greater than maximum");
		return 0;
	}
	if (overflow < 0 || tmp < 0) {
		PyErr_SetString(PyExc_ValueError, "fd is negative");
		return 0;
	}
	*fd = tmp;
	return 1;
}
#endif

int path_converter(PyObject *o, void *p)
{
	struct path_arg *path = p;

	if (o == NULL) {
		path_cleanup(p);
		return 1;
	}

	path->fd = -1;
	path->path = NULL;
	path->length = 0;
	path->bytes = NULL;
	if (path->allow_fd && PyIndex_Check(o)) {
		PyObject *fd_obj;
		int overflow;
		long fd;

		fd_obj = PyNumber_Index(o);
		if (!fd_obj)
			return 0;
		fd = PyLong_AsLongAndOverflow(fd_obj, &overflow);
		Py_DECREF(fd_obj);
		if (fd == -1 && PyErr_Occurred())
			return 0;
		if (overflow > 0 || fd > INT_MAX) {
			PyErr_SetString(PyExc_OverflowError, "fd is greater than maximum");
			return 0;
		}
		if (fd < 0) {
			PyErr_SetString(PyExc_ValueError, "fd is negative");
			return 0;
		}
		path->fd = fd;
	} else {
		if (!PyUnicode_FSConverter(o, &path->bytes)) {
			path->object = path->bytes = NULL;
			return 0;
		}
		path->path = PyBytes_AS_STRING(path->bytes);
		path->length = PyBytes_GET_SIZE(path->bytes);
	}
	Py_INCREF(o);
	path->object = o;
	return Py_CLEANUP_SUPPORTED;
}

PyObject *list_from_uint64_array(const uint64_t *arr, size_t n)
{
    PyObject *ret;
    size_t i;

    ret = PyList_New(n);
    if (!ret)
	    return NULL;

    for (i = 0; i < n; i++) {
	    PyObject *tmp;

	    tmp = PyLong_FromUnsignedLongLong(arr[i]);
	    if (!tmp) {
		    Py_DECREF(ret);
		    return NULL;
	    }
	    PyList_SET_ITEM(ret, i, tmp);
    }

    return ret;
}

void path_cleanup(struct path_arg *path)
{
	Py_CLEAR(path->bytes);
	Py_CLEAR(path->object);
}

static PyMethodDef btrfsutil_methods[] = {
	/* Aliases: sync, fs_sync */
	{"sync", (PyCFunction)filesystem_sync,
	 METH_VARARGS | METH_KEYWORDS,
	 "sync(path)\n\n"
	 "Sync a specific Btrfs filesystem.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"fs_sync", (PyCFunction)filesystem_sync,
	 METH_VARARGS | METH_KEYWORDS,
	 "fs_sync(path)\n\n"
	 "Sync a specific Btrfs filesystem.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},

	/* Aliases: start_sync, fs_start_sync */
	{"start_sync", (PyCFunction)start_sync,
	 METH_VARARGS | METH_KEYWORDS,
	 "start_sync(path) -> int\n\n"
	 "Start a sync on a specific Btrfs filesystem and return the\n"
	 "transaction ID.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"fs_start_sync", (PyCFunction)start_sync,
	 METH_VARARGS | METH_KEYWORDS,
	 "fs_start_sync(path) -> int\n\n"
	 "Start a sync on a specific Btrfs filesystem and return the\n"
	 "transaction ID.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},

	/* Aliases: wait_sync, fs_wait_sync */
	{"wait_sync", (PyCFunction)wait_sync,
	 METH_VARARGS | METH_KEYWORDS,
	 "wait_sync(path, transid=0)\n\n"
	 "Wait for a transaction to sync.\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "transid -- int transaction ID to wait for, or zero for the current\n"
	 "transaction"},
	{"fs_wait_sync", (PyCFunction)wait_sync,
	 METH_VARARGS | METH_KEYWORDS,
	 "fs_wait_sync(path, transid=0)\n\n"
	 "Wait for a transaction to sync.\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "transid -- int transaction ID to wait for, or zero for the current\n"
	 "transaction"},

	/* Aliases: is_subvolume, subvolume_is_valid */
	{"is_subvolume", (PyCFunction)is_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "is_subvolume(path) -> bool\n\n"
	 "Get whether a file is a subvolume.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"subvolume_is_valid", (PyCFunction)is_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_is_valid(path) -> bool\n\n"
	 "Get whether a file is a subvolume.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},

	/* Aliases: subvolume_id, subvolume_get_id */
	{"subvolume_id", (PyCFunction)subvolume_id,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_id(path) -> int\n\n"
	 "Get the ID of the subvolume containing a file.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"subvolume_get_id", (PyCFunction)subvolume_id,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_id(path) -> int\n\n"
	 "Get the ID of the subvolume containing a file.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},

	/* Aliases: subvolume_path, subvolume_get_path */
	{"subvolume_path", (PyCFunction)subvolume_path,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_path(path, id=0) -> int\n\n"
	 "Get the path of a subvolume relative to the filesystem root.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "id -- if not zero, instead of returning the subvolume path of the\n"
	 "given path, return the path of the subvolume with this ID"},
	{"subvolume_get_path", (PyCFunction)subvolume_path,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_get_path(path, id=0) -> int\n\n"
	 "Get the path of a subvolume relative to the filesystem root.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "id -- if not zero, instead of returning the subvolume path of the\n"
	 "given path, return the path of the subvolume with this ID"},

	/* Aliases: subvolume_info, subvolume_get_info */
	{"subvolume_info", (PyCFunction)subvolume_info,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_info(path, id=0) -> SubvolumeInfo\n\n"
	 "Get information about a subvolume.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "id -- if not zero, instead of returning information about the\n"
	 "given path, return information about the subvolume with this ID"},
	{"subvolume_get_info", (PyCFunction)subvolume_info,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_get_info(path, id=0) -> SubvolumeInfo\n\n"
	 "Get information about a subvolume.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "id -- if not zero, instead of returning information about the\n"
	 "given path, return information about the subvolume with this ID"},

	/* Aliases get_subvolume_read_only, subvolume_get_read_only */
	{"get_subvolume_read_only", (PyCFunction)get_subvolume_read_only,
	 METH_VARARGS | METH_KEYWORDS,
	 "get_subvolume_read_only(path) -> bool\n\n"
	 "Get whether a subvolume is read-only.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"subvolume_get_read_only", (PyCFunction)get_subvolume_read_only,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_get_read_only(path) -> bool\n\n"
	 "Get whether a subvolume is read-only.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},

	/* Aliases set_subvolume_read_only, subvolume_set_read_only */
	{"set_subvolume_read_only", (PyCFunction)set_subvolume_read_only,
	 METH_VARARGS | METH_KEYWORDS,
	 "set_subvolume_read_only(path, read_only=True)\n\n"
	 "Set whether a subvolume is read-only.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "read_only -- bool flag value"},
	{"subvolume_set_read_only", (PyCFunction)set_subvolume_read_only,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_set_read_only(path, read_only=True)\n\n"
	 "Set whether a subvolume is read-only.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "read_only -- bool flag value"},

	/* Aliases get_default_subvolume, subvolume_get_default */
	{"get_default_subvolume", (PyCFunction)get_default_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "get_default_subvolume(path) -> int\n\n"
	 "Get the ID of the default subvolume of a filesystem.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"subvolume_get_default", (PyCFunction)get_default_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_get_default(path) -> int\n\n"
	 "Get the ID of the default subvolume of a filesystem.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},

	/* Aliases: set_default_subvolume, subvolume_set_default */
	{"set_default_subvolume", (PyCFunction)set_default_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "set_default_subvolume(path, id=0)\n\n"
	 "Set the default subvolume of a filesystem.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "id -- if not zero, set the default subvolume to the subvolume with\n"
	 "this ID instead of the given path"},
	{"subvolume_set_default", (PyCFunction)set_default_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_set_default(path, id=0)\n\n"
	 "Set the default subvolume of a filesystem.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "id -- if not zero, set the default subvolume to the subvolume with\n"
	 "this ID instead of the given path"},

	/* Aliases: create_subvolume, subvolume_create */
	{"create_subvolume", (PyCFunction)create_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "create_subvolume(path, async_=False, qgroup_inherit=None)\n\n"
	 "Create a new subvolume.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, or path-like object\n"
	 "async_ -- no longer used\n"
	 "qgroup_inherit -- optional QgroupInherit object of qgroups to\n"
	 "inherit from"},
	{"subvolume_create", (PyCFunction)create_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_create(path, async_=False, qgroup_inherit=None)\n\n"
	 "Create a new subvolume.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, or path-like object\n"
	 "async_ -- no longer used\n"
	 "qgroup_inherit -- optional QgroupInherit object of qgroups to\n"
	 "inherit from"},

	/* Aliases: create_snapshot, subvolume_snapshot */
	{"create_snapshot", (PyCFunction)create_snapshot,
	 METH_VARARGS | METH_KEYWORDS,
	 "create_snapshot(source, path, recursive=False, read_only=False,\n"
	 "                async_=False, qgroup_inherit=None)\n\n"
	 "Create a new snapshot.\n\n"
	 "Arguments:\n"
	 "source -- string, bytes, path-like object, or open file descriptor\n"
	 "path -- string, bytes, or path-like object\n"
	 "recursive -- also snapshot child subvolumes\n"
	 "read_only -- create a read-only snapshot\n"
	 "async_ -- no longer used\n"
	 "qgroup_inherit -- optional QgroupInherit object of qgroups to\n"
	 "inherit from"},
	{"subvolume_snapshot", (PyCFunction)create_snapshot,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_snapshot(source, path, recursive=False, read_only=False,\n"
	 "                   async_=False, qgroup_inherit=None)\n\n"
	 "Create a new snapshot.\n\n"
	 "Arguments:\n"
	 "source -- string, bytes, path-like object, or open file descriptor\n"
	 "path -- string, bytes, or path-like object\n"
	 "recursive -- also snapshot child subvolumes\n"
	 "read_only -- create a read-only snapshot\n"
	 "async_ -- no longer used\n"
	 "qgroup_inherit -- optional QgroupInherit object of qgroups to\n"
	 "inherit from"},

	/* Aliases: delete_subvolume, subvolume_delete */
	{"delete_subvolume", (PyCFunction)delete_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "delete_subvolume(path, recursive=False)\n\n"
	 "Delete a subvolume or snapshot.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, or path-like object\n"
	 "recursive -- if the given subvolume has child subvolumes, delete\n"
	 "them instead of failing"},
	{"subvolume_delete", (PyCFunction)delete_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_delete(path, recursive=False)\n\n"
	 "Delete a subvolume or snapshot.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, or path-like object\n"
	 "recursive -- if the given subvolume has child subvolumes, delete\n"
	 "them instead of failing"},

	/* Aliases: deleted_subvolumes, subvolume_list_deleted */
	{"deleted_subvolumes", (PyCFunction)deleted_subvolumes,
	 METH_VARARGS | METH_KEYWORDS,
	 "deleted_subvolumes(path)\n\n"
	 "Get the list of subvolume IDs which have been deleted but not yet\n"
	 "cleaned up\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"subvolume_list_deleted", (PyCFunction)deleted_subvolumes,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolumes_list_deleted(path)\n\n"
	 "Get the list of subvolume IDs which have been deleted but not yet\n"
	 "cleaned up\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},

	{},
};

static struct PyModuleDef btrfsutilmodule = {
	PyModuleDef_HEAD_INIT,
	"btrfsutil",
	"Library for managing Btrfs filesystems",
	-1,
	btrfsutil_methods,
};

PyMODINIT_FUNC
PyInit_btrfsutil(void)
{
	PyObject *m;

	BtrfsUtilError_type.tp_base = (PyTypeObject *)PyExc_OSError;
	if (PyType_Ready(&BtrfsUtilError_type) < 0)
		return NULL;

	if (PyStructSequence_InitType2(&SubvolumeInfo_type, &SubvolumeInfo_desc) < 0)
		return NULL;

	SubvolumeIterator_type.tp_new = PyType_GenericNew;
	if (PyType_Ready(&SubvolumeIterator_type) < 0)
		return NULL;

	QgroupInherit_type.tp_new = PyType_GenericNew;
	if (PyType_Ready(&QgroupInherit_type) < 0)
		return NULL;

	m = PyModule_Create(&btrfsutilmodule);
	if (!m)
		return NULL;

	Py_INCREF(&BtrfsUtilError_type);
	PyModule_AddObject(m, "BtrfsUtilError",
			   (PyObject *)&BtrfsUtilError_type);

	Py_INCREF(&SubvolumeInfo_type);
	PyModule_AddObject(m, "SubvolumeInfo", (PyObject *)&SubvolumeInfo_type);

	Py_INCREF(&SubvolumeIterator_type);
	PyModule_AddObject(m, "SubvolumeIterator",
			   (PyObject *)&SubvolumeIterator_type);

	Py_INCREF(&QgroupInherit_type);
	PyModule_AddObject(m, "QgroupInherit",
			   (PyObject *)&QgroupInherit_type);

	add_module_constants(m);

	return m;
}
