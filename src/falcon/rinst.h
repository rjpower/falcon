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
#define USED_TYPED_REGISTERS 1
#endif

#ifndef STACK_ALLOC_REGISTERS
#define STACK_ALLOC_REGISTERS 1
#endif

#ifndef REUSE_INT_REGISTERS
#define REUSE_INT_REGISTERS 0
#endif

#ifndef PACK_INSTRUCTIONS
#define PACK_INSTRUCTIONS 0
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
#define f_inline __attribute__((noinline))
//#define f_inline __attribute__((always_inline))
#define n_inline __attribute__((noinline))
#endif

const char* obj_to_str(PyObject* o);

static const int kMaxRegisters = 255;
typedef uint8_t RegisterOffset;
static const RegisterOffset kInvalidRegister = (RegisterOffset) -1;

enum RegisterType {
  ObjType = 0,
  IntType = 1,
};

#if USED_TYPED_REGISTERS
struct Register {
  union {
    uint64_t value :63;
    uint64_t type_flag :1;
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

  f_inline PyObject* as_obj() {
    if (get_type() == ObjType) {
      return objval;
    } else {
      objval = PyInt_FromLong(as_int());
//      Log_Info("Coerced: %p %d", thiss, objval->ob_refcnt);
      return objval;
    }
  }

  f_inline RegisterType get_type() const {
    return (RegisterType) type_flag;
  }

  f_inline long as_int() const {
//    Log_Info("load: %p : %d", this, value);
    return value >> 1;
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
    value = v << 1;
    type_flag = IntType;
  }

  f_inline void store(PyObject* obj) {
//    Log_Info("store: %p : %p", this, obj);
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

  f_inline RegisterType get_type() {
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

#if PACK_INSTRUCTIONS
#pragma pack(push, 0)
#endif

struct OpHeader {
  uint8_t code;
  uint8_t arg;
};

struct BranchOp {
  uint8_t code;
  uint8_t arg;
  JumpLoc label;
  RegisterOffset reg[2];

  std::string str(Register* registers = NULL) const;

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

// A variable size instruction can contain any number of registers off the end
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
#if PACK_INSTRUCTIONS
#pragma pack(pop)
#endif

#endif /* RINST_H_ */
