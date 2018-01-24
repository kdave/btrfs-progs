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
	"btrfsutil.QgroupInherit",		/* tp_name */
	sizeof(QgroupInherit),			/* tp_basicsize */
	0,					/* tp_itemsize */
	(destructor)QgroupInherit_dealloc,	/* tp_dealloc */
	NULL,					/* tp_print */
	NULL,					/* tp_getattr */
	NULL,					/* tp_setattr */
	NULL,					/* tp_as_async */
	NULL,					/* tp_repr */
	NULL,					/* tp_as_number */
	NULL,					/* tp_as_sequence */
	NULL,					/* tp_as_mapping */
	NULL,					/* tp_hash  */
	NULL,					/* tp_call */
	NULL,					/* tp_str */
	(getattrofunc)QgroupInherit_getattro,	/* tp_getattro */
	NULL,					/* tp_setattro */
	NULL,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
	QgroupInherit_DOC,			/* tp_doc */
	NULL,					/* tp_traverse */
	NULL,					/* tp_clear */
	NULL,					/* tp_richcompare */
	0,					/* tp_weaklistoffset */
	NULL,					/* tp_iter */
	NULL,					/* tp_iternext */
	QgroupInherit_methods,			/* tp_methods */
	NULL,					/* tp_members */
	NULL,					/* tp_getset */
	NULL,					/* tp_base */
	NULL,					/* tp_dict */
	NULL,					/* tp_descr_get */
	NULL,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	(initproc)QgroupInherit_init,		/* tp_init */
};
