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
//  abort();
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
  Reg_Assert(PyErr_Occurred() != NULL, "No exception to propagate.");
//  PyErr_Fetch(&exception, &value, &traceback);
//  PyErr_Restore(exception, value, traceback);
  exception = value = traceback = NULL;
  file = NULL;
  line = 0;
}

