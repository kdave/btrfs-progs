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

int path_converter(PyObject *o, void *p)
{
	struct path_arg *path = p;
	int is_index, is_bytes, is_unicode;
	PyObject *bytes = NULL;
	Py_ssize_t length = 0;
	char *tmp;

	if (o == NULL) {
		path_cleanup(p);
		return 1;
	}

	path->object = path->cleanup = NULL;
	Py_INCREF(o);

	path->fd = -1;

	is_index = path->allow_fd && PyIndex_Check(o);
	is_bytes = PyBytes_Check(o);
	is_unicode = PyUnicode_Check(o);

	if (!is_index && !is_bytes && !is_unicode) {
		_Py_IDENTIFIER(__fspath__);
		PyObject *func;

		func = _PyObject_LookupSpecial(o, &PyId___fspath__);
		if (func == NULL)
			goto err_format;
		Py_DECREF(o);
		o = PyObject_CallFunctionObjArgs(func, NULL);
		Py_DECREF(func);
		if (o == NULL)
			return 0;
		is_bytes = PyBytes_Check(o);
		is_unicode = PyUnicode_Check(o);
	}

	if (is_unicode) {
		if (!PyUnicode_FSConverter(o, &bytes))
			goto err;
	} else if (is_bytes) {
		bytes = o;
		Py_INCREF(bytes);
	} else if (is_index) {
		if (!fd_converter(o, &path->fd))
			goto err;
		path->path = NULL;
		goto out;
	} else {
err_format:
		PyErr_Format(PyExc_TypeError, "expected %s, not %s",
			     path->allow_fd ? "string, bytes, os.PathLike, or integer" :
			     "string, bytes, or os.PathLike",
			     Py_TYPE(o)->tp_name);
		goto err;
	}

	length = PyBytes_GET_SIZE(bytes);
	tmp = PyBytes_AS_STRING(bytes);
	if ((size_t)length != strlen(tmp)) {
		PyErr_SetString(PyExc_TypeError,
				"path has embedded nul character");
		goto err;
	}

	path->path = tmp;
	if (bytes == o)
		Py_DECREF(bytes);
	else
		path->cleanup = bytes;
	path->fd = -1;

out:
	path->length = length;
	path->object = o;
	return Py_CLEANUP_SUPPORTED;

err:
	Py_XDECREF(o);
	Py_XDECREF(bytes);
	return 0;
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
	Py_CLEAR(path->object);
	Py_CLEAR(path->cleanup);
}

static PyMethodDef btrfsutil_methods[] = {
	{"sync", (PyCFunction)filesystem_sync,
	 METH_VARARGS | METH_KEYWORDS,
	 "sync(path)\n\n"
	 "Sync a specific Btrfs filesystem.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"start_sync", (PyCFunction)start_sync,
	 METH_VARARGS | METH_KEYWORDS,
	 "start_sync(path) -> int\n\n"
	 "Start a sync on a specific Btrfs filesystem and return the\n"
	 "transaction ID.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"wait_sync", (PyCFunction)wait_sync,
	 METH_VARARGS | METH_KEYWORDS,
	 "wait_sync(path, transid=0)\n\n"
	 "Wait for a transaction to sync.\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "transid -- int transaction ID to wait for, or zero for the current\n"
	 "transaction"},
	{"is_subvolume", (PyCFunction)is_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "is_subvolume(path) -> bool\n\n"
	 "Get whether a file is a subvolume.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"subvolume_id", (PyCFunction)subvolume_id,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_id(path) -> int\n\n"
	 "Get the ID of the subvolume containing a file.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"subvolume_path", (PyCFunction)subvolume_path,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_path(path, id=0) -> int\n\n"
	 "Get the path of a subvolume relative to the filesystem root.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "id -- if not zero, instead of returning the subvolume path of the\n"
	 "given path, return the path of the subvolume with this ID"},
	{"subvolume_info", (PyCFunction)subvolume_info,
	 METH_VARARGS | METH_KEYWORDS,
	 "subvolume_info(path, id=0) -> SubvolumeInfo\n\n"
	 "Get information about a subvolume.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "id -- if not zero, instead of returning information about the\n"
	 "given path, return information about the subvolume with this ID"},
	{"get_subvolume_read_only", (PyCFunction)get_subvolume_read_only,
	 METH_VARARGS | METH_KEYWORDS,
	 "get_subvolume_read_only(path) -> bool\n\n"
	 "Get whether a subvolume is read-only.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"set_subvolume_read_only", (PyCFunction)set_subvolume_read_only,
	 METH_VARARGS | METH_KEYWORDS,
	 "set_subvolume_read_only(path, read_only=True)\n\n"
	 "Set whether a subvolume is read-only.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "read_only -- bool flag value"},
	{"get_default_subvolume", (PyCFunction)get_default_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "get_default_subvolume(path) -> int\n\n"
	 "Get the ID of the default subvolume of a filesystem.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor"},
	{"set_default_subvolume", (PyCFunction)set_default_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "set_default_subvolume(path, id=0)\n\n"
	 "Set the default subvolume of a filesystem.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, path-like object, or open file descriptor\n"
	 "id -- if not zero, set the default subvolume to the subvolume with\n"
	 "this ID instead of the given path"},
	{"create_subvolume", (PyCFunction)create_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "create_subvolume(path, async_=False, qgroup_inherit=None)\n\n"
	 "Create a new subvolume.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, or path-like object\n"
	 "async_ -- create the subvolume without waiting for it to commit to\n"
	 "disk and return the transaction ID\n"
	 "qgroup_inherit -- optional QgroupInherit object of qgroups to\n"
	 "inherit from"},
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
	 "async_ -- create the subvolume without waiting for it to commit to\n"
	 "disk and return the transaction ID\n"
	 "qgroup_inherit -- optional QgroupInherit object of qgroups to\n"
	 "inherit from"},
	{"delete_subvolume", (PyCFunction)delete_subvolume,
	 METH_VARARGS | METH_KEYWORDS,
	 "delete_subvolume(path, recursive=False)\n\n"
	 "Delete a subvolume or snapshot.\n\n"
	 "Arguments:\n"
	 "path -- string, bytes, or path-like object\n"
	 "recursive -- if the given subvolume has child subvolumes, delete\n"
	 "them instead of failing"},
	{"deleted_subvolumes", (PyCFunction)deleted_subvolumes,
	 METH_VARARGS | METH_KEYWORDS,
	 "deleted_subvolumes(path)\n\n"
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
