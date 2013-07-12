#ifndef FALCON_BASIC_BLOCK_H
#define FALCON_BASIC_BLOCK_H

#include <vector>

#include "register_stack.h"
#include "compiler_op.h"

struct BasicBlock {
private:
  std::vector<CompilerOp*> alloc_;
  CompilerOp* _add_op(int opcode, int arg, int num_regs);
  CompilerOp* _add_dest_op(int opcode, int arg, int num_regs);
public:

  int py_offset;
  int reg_offset;
  int idx;

  std::vector<BasicBlock*> exits;
  std::vector<BasicBlock*> entries;
  std::vector<CompilerOp*> code;

  // Have we been visited by the current pass already?
  int visited;
  int dead;
  RegisterStack* entry_stack;

  BasicBlock(int offset, int idx, RegisterStack* entry_stack);
  ~BasicBlock() {
    for (auto op : alloc_) {
      delete op;
    }
    delete entry_stack;
  }

  /* operations without a destination register */
  CompilerOp* add_op(int opcode, int arg);
  CompilerOp* add_op(int opcode, int arg, int reg1);
  CompilerOp* add_op(int opcode, int arg, int reg1, int reg2);
  CompilerOp* add_op(int opcode, int arg, int reg1, int reg2, int reg3);
  CompilerOp* add_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4);

  /* operations with a destination register */
  CompilerOp* add_dest_op(int opcode, int arg);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1, int reg2);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4, int reg5);

  CompilerOp* add_varargs_op(int opcode, int arg, int num_regs);
};


#endif
