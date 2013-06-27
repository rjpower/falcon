#include "rlist.h"

// A re-implementation of the Python list object, using registers.
// As you might expect, this shares a lot of similarities with
// listobject.c; unforunately I haven't thought of a great way
// of avoiding all of this reimplementation :/

RListObject* rlist_new() {
  RListObject* rlist = PyObject_New(RListObject, &RList_Type);
  rlist->data = new std::vector<Register>;
  return rlist;
}

RListObject* rlist_from_python(PyObject* pylist) {
  if (RList_CheckExact(pylist)) {
    Py_IncRef(pylist);
    return (RListObject*) pylist;
  }

  RListObject* rlist = rlist_new();
  RefHelper iter = PyObject_GetIter(pylist);
  for (;;) {
    RefHelper v = PyIter_Next(iter);
    if (!v.obj) {
      break;
    }
    rlist->data->push_back(Register(v.obj));
  }
  return rlist;
}

static void rlist_dealloc(RListObject* obj) {
  delete obj->data;
}

static PyObject* rlist_repr(PyObject* obj) {
  return PyString_FromString("RList");
}

static Py_ssize_t rlist_length(RListObject *a) {
  return a->size();
}

PyObject *rlist_concat(PyObject *a, PyObject *b) {
  RListObject *rn = rlist_new();
  RListObject *ra = rlist_from_python(a);
  RListObject *rb = rlist_from_python(b);
  for (auto v : *ra->data) {
    rn->append(v);
  }
  for (auto v : *rb->data) {
    rn->append(v);
  }

  Py_DecRef((PyObject*) ra);
  Py_DecRef((PyObject*) rb);

  return (PyObject*) rn;
}

PyObject *rlist_repeat(PyObject *a, Py_ssize_t n) {
  RListObject *rn = rlist_new();
  for (int i = 0; i < n; ++i) {
    for (auto v : *((RListObject*) a)->data) {
      rn->data->push_back(v);
    }
  }
  return (PyObject*) rn;
}

PyObject* rlist_item(PyObject* a, Py_ssize_t idx) {
  return ((RListObject*) a)->data->at(idx).as_obj();
}

static void fix_bounds(const RListObject& a, Py_ssize_t &ilow, Py_ssize_t &ihigh) {
  if (ilow < 0) {
    ilow = 0;
  }
  if (ilow > a.size()) {
    ilow = a.size();
  }
  if (ihigh < ilow) {
    ihigh = ilow;
  }
  if (ihigh > a.size()) {
    ihigh = a.size();
  }
}

static int check_index(const RListObject& a, PyObject* idx, Py_ssize_t *out) {
  if (!PyInt_Check(idx)) {
    PyErr_Format(PyExc_IndexError, "list indices must be integers, not %.200s", idx->ob_type->tp_name);
    return -1;
  }

  *out = PyInt_AsSsize_t(idx);

  if (*out < 0) {
    *out = a.size() - *out;
  }

  if (*out < 0 || *out >= a.size()) {
    PyErr_SetString(PyExc_IndexError, "list index out of range");
    return -1;
  }

  return 0;
}

PyObject* rlist_slice(PyObject* a, Py_ssize_t ilow, Py_ssize_t ihigh) {
  RListObject& ra = *rlist_from_python(a);
  fix_bounds(ra, ilow, ihigh);
  int len = ihigh - ilow;
  RListObject& rn = *rlist_new();
  rn.data->resize(len);
  for (int i = 0; i < len; ++i) {
    rn[i] = ra[i];
    Py_INCREF(ra[i]);
  }
  return (PyObject*) &rn;
}

int rlist_contains(RListObject* a, PyObject* v) {
  int result;
  for (auto rv : (*a->data)) {
    if (rv.compare(v, &result) == -1) {
      return -1;
    }
    if (result == 0) {
      return 1;
    }
  }
  return 0;
}

int rlist_inplace_concat(PyListObject *self, PyObject *other) {
  PyErr_SetString(PyExc_IndexError, __PRETTY_FUNCTION__);
  return -1;
}

static PyObject *rlist_inplace_repeat(PyListObject *a, Py_ssize_t n) {
  PyErr_SetString(PyExc_IndexError, __PRETTY_FUNCTION__);
  return NULL;
}

static PyObject *rlist_subscript(PyListObject*, PyObject*) {
  PyErr_SetString(PyExc_IndexError, __PRETTY_FUNCTION__);
  return NULL;
}

static int rlist_ass_subscript(PyListObject* self, PyObject* item, PyObject* value) {
  PyErr_SetString(PyExc_IndexError, __PRETTY_FUNCTION__);
  return -1;
}

static PyMappingMethods rlist_as_mapping = { (lenfunc) rlist_length, (binaryfunc) rlist_subscript,
                                             (objobjargproc) rlist_ass_subscript };

int rlist_ass_slice(RListObject* a, Py_ssize_t ilow, Py_ssize_t ihigh, PyObject* v) {
  // TODO(RP) -- implement this properly (V should be a sequence, can be a part of a...)
  PyErr_SetString(PyExc_IndexError, "list slice assignment not implemented.");
  return -1;

  fix_bounds(*a, ilow, ihigh);
  std::vector<Register> garbage;

  garbage.insert(garbage.end(), a->data->begin() + ilow, a->data->begin() + ihigh);
  if (v == NULL) {
    a->data->erase(a->data->begin() + ilow, a->data->begin() + ihigh);
  } else {
    for (int i = ilow; i < ihigh; ++i) {
      (*a)[i] = v;
    }
  }

  for (auto i : garbage) {
    i.decref();
  }
}

int rlist_ass_item(RListObject* a, Py_ssize_t i, PyObject* v) {
  if (i < 0 || i >= a->size()) {
    PyErr_SetString(PyExc_IndexError, "list assignment index out of range");
    return -1;
  }
  if (v == NULL) {
    return rlist_ass_slice(a, i, i + 1, NULL);
  }

  Py_INCREF(v);
  Py_DECREF((*a)[i]);
  (*a)[i] = v;
  return 0;
}

int rlist_clear(PyObject* v) {
  RListObject* rv = (RListObject*) v;
  for (auto r : *rv->data) {
    r.decref();
  }
  rv->data->clear();
  return 0;
}

PyObject* rlist_subscript(PyObject* v, PyObject* idx) {
  RListObject* rv = (RListObject*) v;
  Py_ssize_t idx_v;

  if (check_index(*rv, idx, &idx_v) != 0) {
    return NULL;
  }

  PyObject* out = (*rv)[idx_v];
  Py_INCREF(out);
  return out;
}

static int rlist_traverse(PyListObject *o, visitproc visit, void *arg) {
  PyErr_SetString(PyExc_IndexError, __PRETTY_FUNCTION__);
  return -1;
}

static PyObject *rlist_iter(PyObject *seq) {
  PyErr_SetString(PyExc_IndexError, __PRETTY_FUNCTION__);
  return NULL;
}

static PyObject* rlist_richcompare(PyObject *v, PyObject *w, int op) {
  PyErr_SetString(PyExc_IndexError, __PRETTY_FUNCTION__);
  return NULL;
}

static int rlist_init(PyListObject *self, PyObject *args, PyObject *kw) {
  return 0;
}

static PyMethodDef rlist_methods[] = { { "__getitem__", (PyCFunction) rlist_subscript, METH_O | METH_COEXIST, NULL },
//    {"__reversed__",(PyCFunction)rlist_reversed, METH_NOARGS, reversed_doc},
//    {"__sizeof__",  (PyCFunction)rlist_sizeof, METH_NOARGS, sizeof_doc},
//    {"append",          (PyCFunction)rlistappend,  METH_O, append_doc},
//    {"insert",          (PyCFunction)rlistinsert,  METH_VARARGS, insert_doc},
//    {"extend",      (PyCFunction)rlistextend,  METH_O, extend_doc},
//    {"pop",             (PyCFunction)rlistpop,     METH_VARARGS, pop_doc},
//    {"remove",          (PyCFunction)rlistremove,  METH_O, remove_doc},
//    {"index",           (PyCFunction)rlistindex,   METH_VARARGS, index_doc},
//    {"count",           (PyCFunction)rlistcount,   METH_O, count_doc},
//    {"reverse",         (PyCFunction)rlistreverse, METH_NOARGS, reverse_doc},
//    {"sort",            (PyCFunction)rlistsort,    METH_VARARGS | METH_KEYWORDS, sort_doc},
    { NULL, NULL } /* sentinel */
};

static PySequenceMethods rlist_as_sequence = { (lenfunc) rlist_length, /* sq_length */
                                               (binaryfunc) rlist_concat, /* sq_concat */
                                               (ssizeargfunc) rlist_repeat, /* sq_repeat */
                                               (ssizeargfunc) rlist_item, /* sq_item */
                                               (ssizessizeargfunc) rlist_slice, /* sq_slice */
                                               (ssizeobjargproc) rlist_ass_item, /* sq_ass_item */
                                               (ssizessizeobjargproc) rlist_ass_slice, /* sq_ass_slice */
                                               (objobjproc) rlist_contains, /* sq_contains */
                                               (binaryfunc) rlist_inplace_concat, /* sq_inplace_concat */
                                               (ssizeargfunc) rlist_inplace_repeat, /* sq_inplace_repeat */
};

PyTypeObject RList_Type = {
  PyVarObject_HEAD_INIT(&PyType_Type, 0) "rlist", sizeof(RListObject),
  0,
  (destructor)rlist_dealloc, /* tp_dealloc */
  0, /* tp_print */
  0, /* tp_getattr */
  0, /* tp_setattr */
  0, /* tp_compare */
  (reprfunc)rlist_repr, /* tp_repr */
  0, /* tp_as_number */
  &rlist_as_sequence, /* tp_as_sequence */
  &rlist_as_mapping, /* tp_as_mapping */
  (hashfunc)PyObject_HashNotImplemented, /* tp_hash */
  0, /* tp_call */
  0, /* tp_str */
  PyObject_GenericGetAttr, /* tp_getattro */
  0, /* tp_setattro */
  0, /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_LIST_SUBCLASS, /* tp_flags */
  0, /* tp_doc */
  (traverseproc)rlist_traverse, /* tp_traverse */
  (inquiry)rlist_clear, /* tp_clear */
  rlist_richcompare, /* tp_richcompare */
  0, /* tp_weaklistoffset */
  rlist_iter, /* tp_iter */
  0, /* tp_iternext */
  rlist_methods, /* tp_methods */
  0, /* tp_members */
  0, /* tp_getset */
  0, /* tp_base */
  0, /* tp_dict */
  0, /* tp_descr_get */
  0, /* tp_descr_set */
  0, /* tp_dictoffset */
  (initproc)rlist_init, /* tp_init */
  PyType_GenericAlloc, /* tp_alloc */
  PyType_GenericNew, /* tp_new */
  PyObject_GC_Del, /* tp_free */
};
