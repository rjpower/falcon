#ifndef REVAL_H_
#define REVAL_H_

// Definitions for the register compiler/evaluator.

#include "frameobject.h"

#define REG_MAX_STACK 256
#define REG_MAX_FRAMES 32
#define REG_MAX_BBS 1024

#define REG_MAGIC "REG1"

// Register layout --
//
// Each function call pushes a new set of registers for evaluation.
//
// The first portion of the register file is aliased to the function
// locals and consts.  This is followed by general registers.
// [0..#consts][0..#locals][general registers]
typedef int16_t Register;
typedef int16_t JumpLoc;
typedef void* JumpAddr;

typedef struct {
    int32_t magic;
    int16_t num_registers;
    int16_t mapped_labels :1;
    int16_t mapped_registers :1;
    int16_t reserved :14;
} RegisterPrelude;

typedef struct {
    uint64_t code :8;
    uint64_t arg :16;
    uint64_t reg1 :16;
    uint64_t reg2 :16;
    uint64_t jump :16;
} FixedRegOp;

typedef struct {
    uint8_t code;
    uint16_t arg;
    uint8_t num_registers;
    JumpLoc branches[2];
    Register regs[0];
} VarRegOp;

static inline Py_ssize_t regop_size(const VarRegOp* op) {
    return sizeof(VarRegOp) + sizeof(Register) * op->num_registers;
}

static inline Py_ssize_t regop_size(const FixedRegOp* op) {
    return sizeof(FixedRegOp);
}

PyAPI_FUNC(PyObject*) PyRegEval_EvalFrame(PyFrameObject *f, int throwflag);

#endif /* REVAL_H_ */
