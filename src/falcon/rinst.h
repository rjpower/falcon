#ifndef RINST_H_
#define RINST_H_

#include "Python.h"

#include <string>
#include "oputil.h"
#include "util.h"
#include "rexcept.h"

// These defines enable/disable certain optimizations in the
// evaluator:
#ifndef USED_TYPED_REGISTERS
#define USED_TYPED_REGISTERS 0
#endif

#ifndef STACK_ALLOC_REGISTERS
#define STACK_ALLOC_REGISTERS 1
#endif

#ifndef REUSE_INT_REGISTERS
#define REUSE_INT_REGISTERS 0
#endif


// This file defines the format used by the register evalulator.
//
// Operation types:
//
// There are 3 basic operatoin types: branch, register and varargs.
//
// The register form is used by most operations and is templatized
// on the number of registers used by the operation.
//
// Register layout:
//
// Each function call pushes a new set of registers for evaluation.
//
// The first portion of the register file is aliased to the function
// locals and consts.  This is followed by general registers.
// [0..#consts][0..#locals][general registers]

#if defined(SWIG)
#define f_inline
#define n_inline
#elif defined(FALCON_DEBUG)
#define f_inline __attribute__((noinline))
#define n_inline __attribute__((noinline))
#else
//#define f_inline __attribute__((noinline))
#define f_inline __attribute__((always_inline))
#define n_inline __attribute__((noinline))
#endif

const char* obj_to_str(PyObject* o);

static const int kMaxRegisters = 255;
typedef uint8_t RegisterOffset;
static const RegisterOffset kInvalidRegister = (RegisterOffset) -1;

enum RegisterType {
  IntType,
  FloatType,
  ObjType,
};

#if USED_TYPED_REGISTERS
struct Register {
  RegisterType type;
  union {
    long intval;
    double floatval;
    PyObject* objval;
  };

  f_inline PyObject* as_obj() {
    if (type == ObjType) {
      return objval;
    } else if (type == IntType) {
      type = ObjType;
      objval = PyInt_FromLong(intval);
      return objval;
    } else {
      type = ObjType;
      objval = PyFloat_FromDouble(floatval);
      return objval;
    }
  }

  f_inline RegisterType get_type() const {
    return (RegisterType) type;
  }

  f_inline long as_int() const {
    return intval;
  }

  f_inline double as_float() const {
    return floatval;
  }

  f_inline void decref() {
    if (type == ObjType) {
      Py_XDECREF(objval);
    }
  }

  f_inline void incref() {
    if (type == ObjType) {
      Py_INCREF(objval);
    }
  }

  f_inline void store(PyObject* obj) {
    if (obj == NULL) {
      type = ObjType;
      objval = obj;
    } else if (PyInt_CheckExact(obj)) {
      type = IntType;
      intval = PyInt_AS_LONG(obj);
      Py_DECREF(obj);
    } else if (PyFloat_CheckExact(obj)) {
      type = FloatType;
      floatval = PyFloat_AS_DOUBLE(obj);
      Py_DECREF(obj);
    } else {
      type = ObjType;
      objval = obj;
    }
  }

  f_inline void store(int v) {
    type = IntType;
    intval = v;
  }

  f_inline void store(long v) {
    type = IntType;
    intval = v;
  }

  f_inline void store(double v) {
    type = FloatType;
    floatval = v;
  }
};

#else
struct Register {
  PyObject* v;

  f_inline RegisterType get_type() {
    if (PyInt_CheckExact(v)) {
      return IntType;
    } else if (PyFloat_CheckExact(v)) {
      return FloatType;
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

  f_inline double as_float() {
    return PyFloat_AsDouble(v);
  }

  f_inline void decref() {
    Py_XDECREF(v);
  }

  f_inline void incref() {
    Py_INCREF(v);
  }

  f_inline void store(PyObject* obj) {
    v = obj;
  }

  f_inline void store(int ival) {
    v = PyInt_FromLong(ival);
  }
  f_inline void store(long ival) {
    v = PyInt_FromLong(ival);
  }

  f_inline void store(double fval) {
    v = PyFloat_FromDouble(fval);
  }
};
#endif

typedef uint16_t JumpLoc;
typedef void* JumpAddr;

typedef uint8_t HintOffset;
static const uint16_t kMaxHints = 32;
static const uint16_t kInvalidHint = kMaxHints;

static inline size_t hint_offset(void* obj, void* name) {
  return ((size_t(obj) ^ size_t(name)) >> 4) % kMaxHints;
}

struct RegisterCode {
  int16_t num_registers;
  int16_t version;
  int16_t mapped_labels :1;
  int16_t mapped_registers :1;
  int16_t reserved :14;

  // The Python function object this code object was built from (NULL if
  // compiled directly from a code object).
  PyObject* function;

  PyObject* code_;

  int16_t num_freevars;
  int16_t num_cellvars;
  int16_t num_cells;

  PyCodeObject* code() const {
    return (PyCodeObject*) code_;
  }

  PyObject* names() const {
    return code()->co_names;
  }

  PyObject* varnames() const {
    return code()->co_varnames;
  }

  PyObject* consts() const {
    return code()->co_consts;
  }

  std::string instructions;
};

//#pragma pack(push, 0)
struct OpHeader {
  uint8_t code;
  uint8_t arg;
};

struct BranchOp {
  uint8_t code;
  uint8_t arg;
  JumpLoc label;
  RegisterOffset reg[2];

  std::string str() const;

  inline size_t size() const {
    return sizeof(*this);
  }
};

template<int num_registers>
struct RegOp {
  uint8_t code;
  uint8_t arg;

  // The hint field is used by certain operations to cache information
  // at runtime.  It is default initialized to kInvalidHint;
  HintOffset hint_pos;

  RegisterOffset reg[num_registers];

  std::string str(Register* registers = NULL) const;

  inline size_t size() const {
    return sizeof(*this);
  }
};

// A variable size can contain any number of registers off the end
// of the structure.
struct VarRegOp {
  uint8_t code;
  uint8_t arg;
  uint8_t num_registers;
  RegisterOffset reg[0];

  std::string str(Register* registers = NULL) const;

  inline size_t size() const {
    return sizeof(VarRegOp) + num_registers * sizeof(RegisterOffset);
  }
};
//#pragma pack(pop)

#endif /* RINST_H_ */
