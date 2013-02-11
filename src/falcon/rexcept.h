#ifndef REXCEPT_H_
#define REXCEPT_H_

#include <string>
#include "Python.h"

struct RException {
  PyObject* exception;
  PyObject* value;
  PyObject* traceback;

  void set_python_err() {
    Log_Info("Setting python error: %s %s", obj_to_str(exception), obj_to_str(value));
    PyErr_SetObject(exception, value);
  }

  RException() {
    assert(PyErr_Occurred());
    PyErr_Fetch(&exception, &value, &traceback);
  }

  RException(PyObject* exc, const std::string& err) {
    exception = exc;
    value = PyString_FromString(err.c_str());
    traceback = NULL;
  }

  RException(PyObject* exc, const char* fmt, ...) :
      exception(exc) {
    va_list vargs;
    va_start(vargs, fmt);
    value = PyString_FromFormatV(fmt, vargs);
    va_end(vargs);
    traceback = NULL;
  }
};


#define _ASSERT_OP(op, a, b)\
  { decltype(a) a_ = (a);\
    decltype(b) b_ = (b);\
    if (!(a op b)) {\
      throw RException(PyExc_AssertionError, StringPrintf("Assert failed: expected %s %s %s, got %.0f, %.0f", #a, #op, #b, (double)a_, (double)b_));\
    }\
  }

#define Reg_Assert(cond, msg, ...)\
    { if (!(cond)) { throw RException(PyExc_AssertionError, StringPrintf(msg, ##__VA_ARGS__)); } }

#define Reg_AssertLe(a, b) _ASSERT_OP(<=, a, b)
#define Reg_AssertGe(a, b) _ASSERT_OP(>=, a, b)
#define Reg_AssertEq(a, b) _ASSERT_OP(==, a, b)
#define Reg_AssertGt(a, b) _ASSERT_OP(>, a, b)
#define Reg_AssertLt(a, b) _ASSERT_OP(<, a, b)

#endif /* REXCEPT_H_ */
