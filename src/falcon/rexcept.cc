#include "rexcept.h"
#include "rinst.h"

RException::RException(PyObject* exc, PyObject* value, PyObject* tb) {
  this->exception = exc;
  this->value = value;
  this->traceback = tb;
  this->file = NULL;
  this->line = 0;
}

RException::RException(PyObject* exc, const char* fmt, ...) :
    exception(exc) {
//  breakpoint();
  va_list vargs;
  va_start(vargs, fmt);
  value = PyString_FromFormatV(fmt, vargs);
  va_end(vargs);
  traceback = Py_None;
  Py_INCREF(traceback);
  file = NULL;
  line = 0;
}

RException::RException() {
  assert(PyErr_Occurred());
  traceback = exception = value = Py_None;
  file = NULL;
  line = 0;
}

