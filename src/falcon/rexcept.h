#ifndef REXCEPT_H_
#define REXCEPT_H_

#include "py_include.h"

#include <string>
#include "util.h"

struct RException {
  PyObject* exception;
  PyObject* value;
  PyObject* traceback;

  const char* file;
  int line;

  RException();
  RException(PyObject* exc, const char* fmt, ...);
  RException(PyObject* exc, PyObject* value, PyObject* tb);
};

#define _THROW_ERROR(op, a, b)\
  Log_Fatal("Assert failed (%s:%d): expected %s %s %s, got %s, %s",\
                     __FILE__, __LINE__, #a, #op, #b, Coerce::str(a_).c_str(), Coerce::str(b_).c_str())

#define _ASSERT_OP(op, a, b)\
  { decltype(a) a_ = (a);\
    decltype(b) b_ = (b);\
    if (!(a_ op b_)) {\
      _THROW_ERROR(op, a, b);\
    }\
  }

#define Reg_Assert(cond, msg, ...)\
    { if (!(cond)) { Log_Fatal("Assert failed %s: " #msg, #cond, ##__VA_ARGS__); } }

#define Reg_AssertNe(a, b) _ASSERT_OP(!=, a, b)
#define Reg_AssertLe(a, b) _ASSERT_OP(<=, a, b)
#define Reg_AssertGe(a, b) _ASSERT_OP(>=, a, b)
#define Reg_AssertEq(a, b) _ASSERT_OP(==, a, b)
#define Reg_AssertGt(a, b) _ASSERT_OP(>, a, b)
#define Reg_AssertLt(a, b) _ASSERT_OP(<, a, b)

#endif /* REXCEPT_H_ */
