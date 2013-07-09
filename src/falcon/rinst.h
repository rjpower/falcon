#ifndef RINST_H_
#define RINST_H_

#include "Python.h"

#include <string>

#include "oputil.h"
#include "util.h"
#include "rexcept.h"
#include "config.h"
#include "register.h"

// This file defines the format used by the register evalulator.
//
// Operation types:
//
// There are 3 basic operation types: branch, register and varargs.
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



struct RefHelper {
  PyObject* obj;

  f_inline RefHelper(PyObject* o) :
      obj(o) {
    Py_INCREF(obj);
  }

  f_inline ~RefHelper() {
    Py_DECREF(obj);
  }

  operator PyObject*() {
    return (PyObject*) obj;
  }
};

const char* obj_to_str(PyObject* o);

static const int kMaxRegisters = 256;
typedef uint8_t RegisterOffset;
static const RegisterOffset kInvalidRegister = (RegisterOffset) -1;




typedef uint16_t JumpLoc;
typedef void* JumpAddr;

typedef uint8_t HintOffset;
static const uint8_t kMaxHints = 223;
static const uint8_t kInvalidHint = kMaxHints;

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
  uint16_t arg;
};

template<int kNumRegisters>
struct BranchOp {
  uint8_t code;
  uint16_t arg;
  JumpLoc label;
  RegisterOffset reg[kNumRegisters];

  std::string str(Register* registers = NULL) const;

  inline size_t size() const {
    return sizeof(*this);
  }
};

template<int kNumRegisters>
struct RegOp {
  uint8_t code;
  uint16_t arg;

#if GETATTR_HINTS
  // The hint field is used by certain operations to cache information
  // at runtime.  It is default initialized to kInvalidHint;
  HintOffset hint_pos;
#endif

  RegisterOffset reg[kNumRegisters];

  std::string str(Register* registers = NULL) const;

  inline size_t size() const {
    return sizeof(*this);
  }
};

// A variable size instruction can contain any number of registers off the end
// of the structure.
struct VarRegOp {
  uint8_t code;
  // arg has to be larger than uint8_t because
  // Python uses a weird encoding for keyword arg
  // function calls
  uint16_t arg;
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
