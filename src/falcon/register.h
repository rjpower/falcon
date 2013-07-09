
#ifndef FALCON_REGISTER_H
#define FALCON_REGISTER_H

#include "Python.h"

#include "config.h"
#include "inline.h"

static const int ObjType = 0;
static const int IntType = 1;

#if USED_TYPED_REGISTERS

#define TYPE_MASK 0x1

struct Register {
  union {
    int64_t i_value;
    double f_value;
    PyObject* objval;
  };

  Register() {

  }

  Register(PyObject* v) {
    store(v);
  }

  Register(const Register& r) {
    objval = r.objval;
  }

  operator long() {
    return as_int();
  }

  operator PyObject*() {
    return as_obj();
  }

  int compare(PyObject* v, int *result) {
    if (PyInt_Check(v) && get_type() == IntType) {
      long lv = PyInt_AsLong(v);
      long rv = long(*this);
      if (rv > lv) { *result = 1; }
      else if (rv == lv) { *result = 0; }
      else { *result = -1; }
      return 0;
    }

    return PyObject_Cmp((PyObject*)this, v, result);
  }

  f_inline long as_int() const {
//    Log_Info("load: %p : %d", this, value);
    return i_value >> 1;
  }

  f_inline PyObject*& as_obj() {
    if (get_type() == ObjType) {
      return objval;
    } else {
      objval = PyInt_FromLong(as_int());
//      Log_Info("Coerced: %p %d", thiss, objval->ob_refcnt);
      return objval;
    }
  }

  f_inline void reset() {
    objval = (PyObject*) NULL;
  }

  f_inline int get_type() const {
    return (i_value & TYPE_MASK);
  }

  f_inline void decref() {
    if (get_type() == ObjType) {
//      Log_Info("Decref %p %d", this, objval == NULL ? -1 : objval->ob_refcnt);
      Py_XDECREF(objval);
    }
  }

  f_inline void incref() {
    if (get_type() == ObjType) {
//      Log_Info("Incref %p %d", this, objval == NULL ? -1 : objval->ob_refcnt);
      Py_INCREF(objval);
    }
  }

  f_inline void store(Register& r) {
    objval = r.objval;
  }

  f_inline void store(int v) {
    store((long) v);
  }

  f_inline void store(long v) {
//    Log_Info("store: %p : %d", this, v);
    i_value = v << 1;
    i_value |= IntType;
  }

  f_inline void store(PyObject* obj) {
    if (obj == NULL || !PyInt_CheckExact(obj)) {
      // Type flag is implicitly set to zero as a result of pointer alignment.
      objval = obj;
    } else {
      store(PyInt_AS_LONG(obj) );
      Py_DECREF(obj);
//      Log_Info("%d %d", as_int(), obj->ob_refcnt);
    }
  }
};

#else
struct Register {
  PyObject* v;

  Register() {}
  Register(PyObject* o) : v(o) {}

  f_inline int get_type() {
    if (PyInt_CheckExact(v)) {
      return IntType;
    } else {
      return ObjType;
    }
  }

  f_inline PyObject* as_obj() {
    return v;
  }

  f_inline long as_int() {
    return PyInt_AsLong(v);
  }

  f_inline void decref() {
    Py_XDECREF(v);
  }

  f_inline void incref() {
    Py_INCREF(v);
  }

  f_inline void reset() {
    v = (PyObject*) NULL;
  }
  f_inline void store(PyObject* obj) {
    v = obj;
  }

  f_inline void store(Register& r) {
    v = r.v;
  }

  f_inline void store(int ival) {
    store((long)ival);
  }

  f_inline void store(long ival) {
    v = PyInt_FromLong(ival);
  }
};
#endif

#endif
