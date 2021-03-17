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

static void QgroupInherit_dealloc(QgroupInherit *self)
{
	btrfs_util_destroy_qgroup_inherit(self->inherit);
	Py_TYPE(self)->tp_free((PyObject *)self);
}

static int QgroupInherit_init(QgroupInherit *self, PyObject *args,
			      PyObject *kwds)
{
	static char *keywords[] = {NULL};
	enum btrfs_util_error err;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, ":QgroupInherit",
					 keywords))
		return -1;

	err = btrfs_util_create_qgroup_inherit(0, &self->inherit);
	if (err) {
		SetFromBtrfsUtilError(err);
		return -1;
	}

	return 0;
}

static PyObject *QgroupInherit_getattro(QgroupInherit *self, PyObject *nameobj)
{
    const char *name = "";

    if (PyUnicode_Check(nameobj)) {
	    name = PyUnicode_AsUTF8(nameobj);
	    if (!name)
		    return NULL;
    }

    if (strcmp(name, "groups") == 0) {
	    const uint64_t *arr;
	    size_t n;

	    btrfs_util_qgroup_inherit_get_groups(self->inherit, &arr, &n);

	    return list_from_uint64_array(arr, n);
    } else {
	    return PyObject_GenericGetAttr((PyObject *)self, nameobj);
    }
}

static PyObject *QgroupInherit_add_group(QgroupInherit *self, PyObject *args,
					 PyObject *kwds)
{
	static char *keywords[] = {"qgroupid", NULL};
	enum btrfs_util_error err;
	uint64_t qgroupid;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "K:add_group", keywords,
					 &qgroupid))
		return NULL;

	err = btrfs_util_qgroup_inherit_add_group(&self->inherit, qgroupid);
	if (err) {
		SetFromBtrfsUtilError(err);
		return NULL;
	}

	Py_RETURN_NONE;
}

static PyMethodDef QgroupInherit_methods[] = {
	{"add_group", (PyCFunction)QgroupInherit_add_group,
	 METH_VARARGS | METH_KEYWORDS,
	 "add_group(qgroupid)\n\n"
	 "Add a qgroup to inherit from.\n\n"
	 "Arguments:\n"
	 "qgroupid -- ID of qgroup to add"},
	{},
};

#define QgroupInherit_DOC	\
	"QgroupInherit() -> new qgroup inheritance specifier\n\n"	\
	"Create a new object which specifies what qgroups to inherit\n"	\
	"from for create_subvolume() and create_snapshot()"

PyTypeObject QgroupInherit_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name		= "btrfsutil.QgroupInherit",
	.tp_basicsize		= sizeof(QgroupInherit),
	.tp_dealloc		= (destructor)QgroupInherit_dealloc,
	.tp_getattro		= (getattrofunc)QgroupInherit_getattro,
	.tp_flags		= Py_TPFLAGS_DEFAULT,
	.tp_doc			= QgroupInherit_DOC,
	.tp_methods		= QgroupInherit_methods,
	.tp_init		= (initproc)QgroupInherit_init,
};
