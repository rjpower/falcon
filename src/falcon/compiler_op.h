#ifndef FALCON_COMPILER_OP_H
#define FALCON_COMPILER_OP_H

#include <vector>
#include <string>

#include "rexcept.h"

#define COMPILE_LOG(...) do { if (getenv("COMPILE_LOG")) { Log_Info(__VA_ARGS__); } } while (0)

// While compiling, we use an expanded form to represent opcodes.  This
// is translated to a compact instruction stream as the last compilation
// step.
struct CompilerOp {
  int code;
  int arg;

  // this instruction has been marked dead by an optimization pass,
  // and should be ignored.
  bool dead;

  // is the last register argument a destination we're writing to?
  bool has_dest;

  std::vector<int> regs;

  std::string str() const;

  CompilerOp(int code, int arg) {
    this->code = code;
    this->arg = arg;
    this->dead = false;
    this->has_dest = false;
  }

  int dest() {
    size_t n_regs = this->regs.size();
    Reg_Assert(n_regs > 0, "Expected registers on operation");
    Reg_Assert(this->has_dest, "Expected to have destination register");
    return this->regs[n_regs - 1];
  }

  size_t num_inputs() {
    size_t n = this->regs.size();
    // if one of the registers is a target for a store, don't count it as an input
    return this->has_dest ? n - 1 : n;
  }
};

#endif
