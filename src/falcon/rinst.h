#ifndef RINST_H_
#define RINST_H_

#include "Python.h"

#include <string>
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
//#define f_inline __attribute__((noinline))
#define f_inline __attribute__((always_inline))
#define n_inline __attribute__((noinline))
#endif

const char* obj_to_str(PyObject* o);


typedef uint8_t Register;

typedef uint16_t JumpLoc;
typedef void* JumpAddr;

static const Register kInvalidRegister = (Register) -1;

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
  HintOffset hint_pos;

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

#pragma pack(pop)

#endif /* RINST_H_ */
