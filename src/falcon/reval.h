#ifndef REVAL_H_
#define REVAL_H_

// Definitions for the register compiler/evaluator.
#include "frameobject.h"
#include "util.h"
#include "oputil.h"

#include <algorithm>
#include <set>

#define REG_MAX_STACK 256
#define REG_MAX_FRAMES 32
#define REG_MAX_BBS 1024

#define REG_MAGIC "REG1"

#ifdef SWIG
#define f_inline
#else
//#define f_inline __attribute__((noinline)) inline
#define f_inline __attribute__((always_inline)) inline
#endif

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

static const uint8_t kBadRegister = (uint8_t)-1;

#pragma pack(push, 0)
struct RegisterPrelude {
  int32_t magic;
  int16_t num_registers;
  int16_t mapped_labels :1;
  int16_t mapped_registers :1;
  int16_t reserved :14;
};

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
};

struct RegOp {
  uint8_t code;
  uint8_t arg;
  Register reg_1;
  Register reg_2;
  Register reg_3;
  Register reg_4;
};

// A variable size op is at least 8 bytes, but can contain
// addition registers off the end of the structure.
struct VarRegOp {
  uint8_t code;
  uint8_t arg;
  uint8_t num_registers;
  Register regs[2];
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
    return RMachineOp::size(*this);
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

struct RegisterFrame {
  PyFrameObject* frame;
  PyObject* regcode;
  RegisterFrame(PyFrameObject* f, PyObject* c) :
      frame(f), regcode(c) {
  }
  ~RegisterFrame() {
    Py_XDECREF(frame);
  }
};

class Evaluator;

struct RunState {
  char *code;
  PyFrameObject* frame;
  RegisterPrelude prelude;
  PyObject *call_args;
  Evaluator* eval;

  f_inline PyObject* names() {
    return frame->f_code->co_names;
  }

  f_inline PyObject* locals() {
    return frame->f_locals;
  }

  f_inline PyObject* globals() {
    return frame->f_globals;
  }

  f_inline PyObject* builtins() {
    return frame->f_builtins;
  }

  f_inline PyObject* consts() {
    return frame->f_code->co_consts;
  }

  f_inline int offset(const char* pc) const {
    return (int) (pc - code);
  }

  f_inline int next_code(const char* pc) const {
    return ((RMachineOp*) pc)->header.code;
  }

  RunState(RegisterFrame* r) {
    Py_ssize_t codelen;
    PyString_AsStringAndSize(r->regcode, &code, &codelen);

    frame = r->frame;
    call_args = NULL;

    prelude = *(RegisterPrelude*) code;
    Log_AssertEq(memcmp(&prelude.magic, REG_MAGIC, 4), 0);
  }
};

class Evaluator {
private:
  f_inline void collectInfo(int opcode);
public:
  int32_t opCounts[256];
  int64_t opTimes[256];

  int32_t totalCount;
  int64_t lastClock;

  Evaluator() {
    bzero(opCounts, sizeof(opCounts));
    bzero(opTimes, sizeof(opTimes));
    totalCount = 0;
    lastClock = 0;
  }

  PyObject* eval(RegisterFrame* rf);
  PyObject* eval_python(PyObject* func, PyObject* args);

  RegisterFrame* frame_from_python(PyObject* func, PyObject* args);
  RegisterFrame* frame_from_regcode(PyObject* code);

  void dumpStatus();
};

#endif /* REVAL_H_ */
