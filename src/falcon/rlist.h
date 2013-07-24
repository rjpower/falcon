#ifndef FALCON_RLIST_H
#define FALCON_RLIST_H

#include "py_include.h"
#include "rinst.h"

// A specialized list object which holds Registers.
// Supports the normal Python list protocol in addition to allowing access
// directly to the internal register data.
typedef struct {
  PyObject_VAR_HEAD
  std::vector<Register> *data;

  PyObject*& operator[](int idx) {
    return (*data)[idx].as_obj();
  }

  Py_ssize_t size() const {
    return data->size();
  }

  void append(PyObject* o) {
    data->push_back(o);
  }

  void append(Register& o) {
    data->push_back(o);
  }

} RListObject;

PyAPI_DATA(PyTypeObject) RList_Type;

#define RList_CheckExact(op) (Py_TYPE(op) == &RList_Type)

RListObject* rlist_new();
RListObject* rlist_from_python(PyObject* list);

#endif /* FALCON_RLIST_H */
