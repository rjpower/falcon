

#include "basic_block.h"
#include "rexcept.h"

CompilerOp* BasicBlock::_add_op(int opcode, int arg, int num_regs) {
  CompilerOp* op = new CompilerOp(opcode, arg);
  op->regs.resize(num_regs);
  alloc_.push_back(op);
  code.push_back(op);
  return op;
}

CompilerOp* BasicBlock::_add_dest_op(int opcode, int arg, int num_regs) {
  CompilerOp* op = _add_op(opcode, arg, num_regs);
  op->has_dest = true;
  return op;
}

BasicBlock::BasicBlock(int offset, int idx, RegisterStack* entry_stack) {
  reg_offset = 0;
  py_offset = offset;
  visited = 0;
  dead = false;
  this->idx = idx;
  this->entry_stack = entry_stack;
}

CompilerOp* BasicBlock::add_op(int opcode, int arg) {
  /* operation with 0 inputs and no destination register */
  return _add_op(opcode, arg, 0);
}

CompilerOp* BasicBlock::add_op(int opcode, int arg, int reg1) {
  /* operation with 1 input and no destination register */
  CompilerOp* op = _add_op(opcode, arg, 1);
  op->regs[0] = reg1;
  return op;
}

CompilerOp* BasicBlock::add_op(int opcode, int arg, int reg1, int reg2) {
  /* operation with 2 inputs and no destination register */
  CompilerOp* op = _add_op(opcode, arg, 2);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  return op;
}

CompilerOp* BasicBlock::add_op(int opcode, int arg, int reg1, int reg2, int reg3) {
  /* operation with 3 inputs and no destination register */
  CompilerOp* op = _add_op(opcode, arg, 3);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  op->regs[2] = reg3;
  return op;
}

CompilerOp* BasicBlock::add_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4) {
  /* operation with 4 inputs and no destination register */
  CompilerOp* op = _add_op(opcode, arg, 4);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  op->regs[2] = reg3;
  op->regs[3] = reg4;
  return op;
}

CompilerOp* BasicBlock::add_dest_op(int opcode, int arg) {
  /* operation with 0 inputs and a destination register */
  return _add_dest_op(opcode, arg, 0);
}

CompilerOp* BasicBlock::add_dest_op(int opcode, int arg, int reg1) {
  /* operation with 1 input and a destination register */
  CompilerOp* op = _add_dest_op(opcode, arg, 1);
  op->regs[0] = reg1;
  return op;
}

CompilerOp* BasicBlock::add_dest_op(int opcode, int arg, int reg1, int reg2) {
  /* operation with 2 inputs and a destination register */
  CompilerOp* op = _add_dest_op(opcode, arg, 2);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  return op;
}

CompilerOp* BasicBlock::add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3) {
  /* operation with 3 inputs and a destination register */
  CompilerOp* op = _add_dest_op(opcode, arg, 3);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  op->regs[2] = reg3;
  return op;
}

CompilerOp* BasicBlock::add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4) {
  /* operation with 4 inputs and a destination register */
  CompilerOp* op = _add_dest_op(opcode, arg, 4);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  op->regs[2] = reg3;
  op->regs[3] = reg4;
  return op;
}

CompilerOp* BasicBlock::add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4, int reg5) {
  /* operation with 5 inputs and a destination register */
  CompilerOp* op = _add_dest_op(opcode, arg, 5);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  op->regs[2] = reg3;
  op->regs[3] = reg4;
  op->regs[4] = reg5;
  return op;
}

CompilerOp* BasicBlock::add_varargs_op(int opcode, int arg, int num_regs) {
  return _add_dest_op(opcode, arg, num_regs);
}
