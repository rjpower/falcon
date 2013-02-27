#include "rexcept.h"
#include "rinst.h"

RException::RException(PyObject* exc, const char* fmt, ...) :
    exception(exc) {
//  breakpoint();
  va_list vargs;
  va_start(vargs, fmt);
  value = PyString_FromFormatV(fmt, vargs);
  va_end(vargs);
  traceback = NULL;
  file = NULL;
  line = 0;
}

RException::RException() {
  assert(PyErr_Occurred());
  traceback = exception = value = NULL;
  file = NULL;
  line = 0;
}

