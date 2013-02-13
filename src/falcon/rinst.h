#ifndef RINST_H_
#define RINST_H_

#include <string>
#include "Python.h"
#include "oputil.h"
#include "util.h"

#if defined(SWIG)
#define f_inline
#elif defined(FALCON_DEBUG)
#define f_inline __attribute__((noinline))
#else
#define f_inline __attribute__((always_inline))
#endif

static inline const char* obj_to_str(PyObject* o) {
  return PyString_AsString(PyObject_Str(o));
}

// Register layout --
//
// Each function call pushes a new set of registers for evaluation.
//
// The first portion of the register file is aliased to the function
// locals and consts.  This is followed by general registers.
// [0..#consts][0..#locals][general registers]
typedef uint8_t Register;
typedef uint16_t JumpLoc;
typedef void* JumpAddr;

static const uint8_t kInvalidRegister = (uint8_t) -1;

struct RegisterCode {
  int16_t num_registers;
  int16_t version;
  int16_t mapped_labels :1;
  int16_t mapped_registers :1;
  int16_t reserved :14;

  // The function this code is derived from.
  PyObject* function;

  PyCodeObject* code() const {
    return (PyCodeObject*) PyFunction_GET_CODE(function);
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

  std::string str() const {
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

  f_inline size_t size() const {
    return sizeof(*this);
  }
};

template <int num_registers>
struct RegOp {
  uint8_t code;
  uint8_t arg;
  Register reg[num_registers];

  std::string str() const {
    StringWriter w;
    w.printf("%s[%d] (", OpUtil::name(code), arg);
    for (int i = 0; i < num_registers; ++i) {
      w.printf("%d,", reg[i]);
    }
    w.printf(")");
    return w.str();
  }

  f_inline size_t size() const {
    return sizeof(*this);
  }
};

// A variable size can contain any number of registers off the end
// of the structure.
struct VarRegOp {
  uint8_t code;
  uint8_t arg;
  uint8_t num_registers;
  Register regs[0];

  std::string str() const {
    StringWriter w;
    w.printf("%s[%d] (%s)", OpUtil::name(code), arg, StrUtil::join(&regs[0], &regs[num_registers]).c_str());
    return w.str();
  }

  f_inline size_t size() const {
    return sizeof(VarRegOp) + num_registers * sizeof(Register);
  }
};

#pragma pack(pop)

#endif /* RINST_H_ */
