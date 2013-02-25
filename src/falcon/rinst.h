#ifndef RINST_H_
#define RINST_H_

#include <string>
#include "Python.h"
#include "oputil.h"
#include "util.h"

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
// #define f_inline __attribute__((noinline))
#define f_inline __attribute__((always_inline))
#define n_inline __attribute__((noinline))
#endif

static inline const char* obj_to_str(PyObject* o) {
  if (o == NULL) {
    return "<NULL>";
  }
  if (PyString_Check(o)) {
    return PyString_AsString(o);
  }

  PyObject* obj_repr = PyObject_Repr(o);
  if (obj_repr == NULL) {
    return "<INVALID __repr__>";
  }
  return PyString_AsString(obj_repr);
}

typedef uint16_t Register;
typedef uint16_t JumpLoc;
typedef void* JumpAddr;

static const Register kInvalidRegister = (Register) -1;

typedef uint16_t HintOffset;
static const uint16_t kMaxHints = 1024;
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

#pragma pack(push, 0)
struct OpHeader {
  uint8_t code;
  uint8_t arg;
};

struct BranchOp {
  uint8_t code;
  uint8_t arg;
  JumpLoc label;
  Register reg[2];

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
  HintOffset hint;

  Register reg[num_registers];

  std::string str(PyObject** registers = NULL) const;

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
  Register reg[0];

  std::string str(PyObject** registers = NULL) const;

  inline size_t size() const {
    return sizeof(VarRegOp) + num_registers * sizeof(Register);
  }
};

template<int num_registers>
inline std::string RegOp<num_registers>::str(PyObject** registers) const {
  StringWriter w;
  w.printf("%s.%d (", OpUtil::name(code), arg);
  for (int i = 0; i < num_registers; ++i) {
    if (registers == NULL) {
      w.printf("%d,", reg[i]);
    } else {
      w.printf("[%d] %.20s, ", reg[i], obj_to_str(registers[reg[i]]));
    }
  }
  w.printf(")");
  return w.str();
}

inline std::string VarRegOp::str(PyObject** registers) const {
  StringWriter w;
  w.printf("%s.%d (", OpUtil::name(code), arg);
  for (int i = 0; i < num_registers; ++i) {
    if (registers == NULL) {
      w.printf("%d,", reg[i]);
    } else {
      w.printf("[%d] %.20s, ", reg[i], obj_to_str(registers[reg[i]]));
    }
  }
  w.printf(")");
  return w.str();
}

inline std::string BranchOp::str() const {
  StringWriter w;
  w.printf("%s (", OpUtil::name(code));
  if (reg[0] != kInvalidRegister) {
    w.printf("%d, ", reg[0]);
  }
  if (reg[1] != kInvalidRegister) {
    w.printf("%d, ", reg[1]);
  }
  w.printf(")");
  w.printf(" -> [%d]", label);
  return w.str();
}

#pragma pack(pop)

#endif /* RINST_H_ */
