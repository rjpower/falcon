#include "rexcept.h"
#include "rinst.h"

void RException::set_python_err() {
  Log_Info("Setting python error: %s %s", obj_to_str(exception), obj_to_str(value));
  PyErr_SetObject(exception, value);
}

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
  PyErr_Fetch(&exception, &value, &traceback);
  file = NULL;
  line = 0;
}

