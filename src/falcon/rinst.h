#ifndef RINST_H_
#define RINST_H_

#include <string>
#include "Python.h"
#include "oputil.h"
#include "util.h"

#if defined(SWIG)
#define f_inline
#elif defined(FALCON_DEBUG)
#define f_inline
#else
#define f_inline __attribute__((always_inline)) inline
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

static const uint8_t kBadRegister = (uint8_t) -1;

struct RegisterCode {
  int16_t num_registers;
  int16_t version;
  int16_t mapped_labels :1;
  int16_t mapped_registers :1;
  int16_t reserved :14;

  // The function this code is derived from.
  PyObject* function;

  PyCodeObject* code() const {
    return (PyCodeObject*) PyFunction_GetCode(function);
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
  Register reg_1;
  Register reg_2;
  JumpLoc label;

  std::string str() const {
    StringWriter w;
    w.printf("%s (", OpUtil::name(code));
    if (reg_1 != kBadRegister) {
      w.printf("%d, ", reg_1);
    }
    if (reg_2 != kBadRegister) {
      w.printf("%d, ", reg_2);
    }
    w.printf(")");
    w.printf(" -> [%d]", label);
    return w.str();
  }
};

struct RegOp {
  uint8_t code;
  uint8_t arg;
  Register reg_1;
  Register reg_2;
  Register reg_3;
  Register reg_4;

  std::string str() const {
    StringWriter w;
    w.printf("%s[%d] (", OpUtil::name(code), arg);
    if (reg_1 != kBadRegister) {
      w.printf("%d, ", reg_1);
    }
    if (reg_2 != kBadRegister) {
      w.printf("%d, ", reg_2);
    }
    if (reg_3 != kBadRegister) {
      w.printf("%d, ", reg_3);
    }
    if (reg_4 != kBadRegister) {
      w.printf("%d, ", reg_4);
    }
    w.printf(")");
    return w.str();
  }
};

// A variable size op is at least 8 bytes, but can contain
// addition registers off the end of the structure.
struct VarRegOp {
  uint8_t code;
  uint8_t arg;
  uint8_t num_registers;
  Register regs[2];

  std::string str() const {
    StringWriter w;
    w.printf("%s[%d] (%s)", OpUtil::name(code), arg, StrUtil::join(&regs[0], &regs[num_registers]).c_str());
    return w.str();
  }
};

struct RMachineOp {
  union {
    OpHeader header;
    BranchOp branch;
    RegOp reg;
    VarRegOp varargs;
  };

  inline uint8_t code() {
    return header.code;
  }

  inline uint8_t arg() {
    return header.arg;
  }

  inline int size() {
    return size(*this);
  }

  static f_inline Py_ssize_t size(const VarRegOp& op) {
    return sizeof(RMachineOp) + sizeof(Register) * std::max(0, op.num_registers - 2);
  }

  static f_inline Py_ssize_t size(const RegOp& op) {
    return sizeof(RMachineOp);
  }

  static f_inline Py_ssize_t size(const BranchOp& op) {
    return sizeof(RMachineOp);
  }

  static f_inline Py_ssize_t size(const RMachineOp& op) {
    assert(sizeof(RMachineOp) == 8);
    assert(sizeof(VarRegOp) == sizeof(RMachineOp));

    if (OpUtil::is_varargs(op.header.code)) {
      return size(op.varargs);
    }
    if (OpUtil::is_branch(op.header.code)) {
      return sizeof(RMachineOp);
    }
    return sizeof(RMachineOp);
  }
};
#pragma pack(pop)

#endif /* RINST_H_ */
