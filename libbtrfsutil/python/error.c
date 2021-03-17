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

typedef struct {
	PyOSErrorObject os_error;
	PyObject *btrfsutilerror;
} BtrfsUtilError;

void SetFromBtrfsUtilError(enum btrfs_util_error err)
{
	SetFromBtrfsUtilErrorWithPaths(err, NULL, NULL);
}

void SetFromBtrfsUtilErrorWithPath(enum btrfs_util_error err,
				   struct path_arg *path1)
{
	SetFromBtrfsUtilErrorWithPaths(err, path1, NULL);
}

void SetFromBtrfsUtilErrorWithPaths(enum btrfs_util_error err,
				    struct path_arg *path1,
				    struct path_arg *path2)
{
	PyObject *strobj, *args, *exc;
	int i = errno;
	const char *str1 = btrfs_util_strerror(err), *str2 = strerror(i);

	if (str1 && str2 && strcmp(str1, str2) != 0) {
		strobj = PyUnicode_FromFormat("%s: %s", str1, str2);
	} else if (str1) {
		strobj = PyUnicode_FromString(str1);
	} else if (str2) {
		strobj = PyUnicode_FromString(str2);
	} else {
		Py_INCREF(Py_None);
		strobj = Py_None;
	}
	if (strobj == NULL)
		return;

	args = Py_BuildValue("iOOOOi", i, strobj,
			     path1 ? path1->object : Py_None, Py_None,
			     path2 ? path2->object : Py_None, (int)err);
	Py_DECREF(strobj);
	if (args == NULL)
		return;

	exc = PyObject_CallObject((PyObject *)&BtrfsUtilError_type, args);
	Py_DECREF(args);
	if (exc == NULL)
		return;

	PyErr_SetObject((PyObject *)&BtrfsUtilError_type, exc);
	Py_DECREF(exc);
}

static int BtrfsUtilError_clear(BtrfsUtilError *self)
{
	Py_CLEAR(self->btrfsutilerror);
	return Py_TYPE(self)->tp_base->tp_clear((PyObject *)self);
}

static void BtrfsUtilError_dealloc(BtrfsUtilError *self)
{
	PyObject_GC_UnTrack(self);
	BtrfsUtilError_clear(self);
	Py_TYPE(self)->tp_free((PyObject *)self);
}

static int BtrfsUtilError_traverse(BtrfsUtilError *self, visitproc visit,
				   void *arg)
{
	Py_VISIT(self->btrfsutilerror);
	return Py_TYPE(self)->tp_base->tp_traverse((PyObject *)self, visit, arg);
}

static PyObject *BtrfsUtilError_new(PyTypeObject *type, PyObject *args,
				    PyObject *kwds)
{
	BtrfsUtilError *self;
	PyObject *oserror_args = args;

	if (PyTuple_Check(args) && PyTuple_GET_SIZE(args) == 6) {
		oserror_args = PyTuple_GetSlice(args, 0, 5);
		if (oserror_args == NULL)
			return NULL;
	}

	self = (BtrfsUtilError *)type->tp_base->tp_new(type, oserror_args,
						       kwds);
	if (oserror_args != args)
		Py_DECREF(oserror_args);
	if (self == NULL)
		return NULL;

	if (PyTuple_Check(args) && PyTuple_GET_SIZE(args) == 6) {
		self->btrfsutilerror = PyTuple_GET_ITEM(args, 5);
		Py_INCREF(self->btrfsutilerror);
	}

	return (PyObject *)self;
}

static PyObject *BtrfsUtilError_str(BtrfsUtilError *self)
{
#define OR_NONE(x) ((x) ? (x) : Py_None)
	if (self->btrfsutilerror) {
		if (self->os_error.filename) {
			if (self->os_error.filename2) {
				return PyUnicode_FromFormat("[BtrfsUtilError %S Errno %S] %S: %R -> %R",
							    OR_NONE(self->btrfsutilerror),
							    OR_NONE(self->os_error.myerrno),
							    OR_NONE(self->os_error.strerror),
							    self->os_error.filename,
							    self->os_error.filename2);
			} else {
				return PyUnicode_FromFormat("[BtrfsUtilError %S Errno %S] %S: %R",
							    OR_NONE(self->btrfsutilerror),
							    OR_NONE(self->os_error.myerrno),
							    OR_NONE(self->os_error.strerror),
							    self->os_error.filename);
			}
		}
		if (self->os_error.myerrno && self->os_error.strerror) {
			return PyUnicode_FromFormat("[BtrfsUtilError %S Errno %S] %S",
						    self->btrfsutilerror,
						    self->os_error.myerrno,
						    self->os_error.strerror);
		}
	}
	return Py_TYPE(self)->tp_base->tp_str((PyObject *)self);
#undef OR_NONE
}

static PyMemberDef BtrfsUtilError_members[] = {
	{"btrfsutilerror", T_OBJECT,
	 offsetof(BtrfsUtilError, btrfsutilerror), 0,
	 "btrfsutil error code"},
	{},
};

#define BtrfsUtilError_DOC	\
	"Btrfs operation error."

PyTypeObject BtrfsUtilError_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name		= "btrfsutil.BtrfsUtilError",
	.tp_basicsize		= sizeof(BtrfsUtilError),
	.tp_dealloc		= (destructor)BtrfsUtilError_dealloc,
	.tp_str			= (reprfunc)BtrfsUtilError_str,
	.tp_flags		=  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
				   Py_TPFLAGS_HAVE_GC,
	.tp_doc			= BtrfsUtilError_DOC,
	.tp_traverse		= (traverseproc)BtrfsUtilError_traverse,
	.tp_clear		= (inquiry)BtrfsUtilError_clear,
	.tp_members		= BtrfsUtilError_members,
	.tp_dictoffset		= offsetof(BtrfsUtilError, os_error.dict),
	.tp_new			= BtrfsUtilError_new,
};
