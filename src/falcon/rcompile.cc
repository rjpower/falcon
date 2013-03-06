#include "Python.h"

#include "Python-ast.h"
#include "node.h"
#include "pyarena.h"
#include "ast.h"
#include "code.h"
#include "compile.h"
#include "symtable.h"
#include "marshal.h"
#include "opcode.h"

#include "rcompile.h"
#include "reval.h"
#include "util.h"

#include <string.h>
#include <stdint.h>

#include <algorithm>
#include <map>
#include <set>
#include <stack>
#include <queue>
#include <vector>
#include <string>

#define GETARG(arr, i) ((int)((arr[i+2]<<8) + arr[i+1]))
#define CODESIZE(op)  (HAS_ARG(op) ? 3 : 1)
#define COMPILE_LOG(...) do { if (getenv("COMPILE_LOG")) { Log_Info(__VA_ARGS__); } } while (0)

using namespace std;

void RegisterStack::push_frame(int target) {
  Frame f;
  f.stack_pos = regs.size();
  f.target = target;
  frames.push_back(f);
}

Frame RegisterStack::pop_frame() {
  Frame f = frames.back();
  frames.pop_back();
  regs.resize(f.stack_pos);
  return f;
}

int RegisterStack::push_register(int reg) {
  // Log_Info("Pushing register %d, pos %d", reg, stack_pos + 1);
  regs.push_back(reg);
  return reg;
}

int RegisterStack::pop_register() {
  Reg_AssertGt((int)regs.size(), 0);
  int reg = regs.back();
  regs.pop_back();
  return reg;
}

// Implement ceval's slightly odd PEEK semantics.  To wit: offset
// zero is invalid, the offset of the top register on the stack is
// 1.
int RegisterStack::peek_register(int offset) {
  Reg_AssertGt(offset, 0);
  Reg_AssertGe((int)regs.size(), offset);
  int val = regs[regs.size() - offset];
//  Log_Info("Peek: %d = %d", offset, val);
  return val;
}

struct RCompilerUtil {
  static int op_size(CompilerOp* op) {
    if (OpUtil::is_varargs(op->code)) {
      return sizeof(VarRegOp) + sizeof(RegisterOffset) * op->regs.size();
    } else if (OpUtil::is_branch(op->code)) {
      return sizeof(BranchOp);
    } else if (op->regs.size() == 0) {
      return sizeof(RegOp<0> );
    } else if (op->regs.size() == 1) {
      return sizeof(RegOp<1> );
    } else if (op->regs.size() == 2) {
      return sizeof(RegOp<2> );
    } else if (op->regs.size() == 3) {
      return sizeof(RegOp<3> );
    } else if (op->regs.size() == 4) {
      return sizeof(RegOp<4> );
    }
    throw RException(PyExc_AssertionError, "Invalid op type.");
    return -1;
  }

  // Lower an operation from compilerop to instruction stream form.
  static void lower_op(char* dst, CompilerOp* src) {
    OpHeader* header = (OpHeader*) dst;
    header->code = src->code;
    header->arg = src->arg;

    if (OpUtil::is_varargs(src->code)) {
      VarRegOp* op = (VarRegOp*) dst;
      op->num_registers = src->regs.size();
      for (size_t i = 0; i < src->regs.size(); ++i) {
        op->reg[i] = src->regs[i];

        // Guard against overflowing our register size.
        Reg_AssertEq(op->reg[i], src->regs[i]);
      }
      Reg_AssertEq(op->num_registers, src->regs.size());
    } else if (OpUtil::is_branch(src->code)) {
      BranchOp* op = (BranchOp*) dst;
      assert(src->regs.size() < 3);
      op->reg[0] = src->regs.size() > 0 ? src->regs[0] : kInvalidRegister;
      op->reg[1] = src->regs.size() > 1 ? src->regs[1] : kInvalidRegister;

      // Label be set after the first pass has determined the offset
      // of each instruction.
      op->label = 0;
    } else {
      Reg_AssertLe(src->regs.size(), 4);
      RegOp<0>* op = (RegOp<0>*) dst;
      for (size_t i = 0; i < src->regs.size(); ++i) {
//        op->reg.set(i, src->regs[i]);
        op->reg[i] = src->regs[i];
      }
      op->hint_pos = kInvalidHint;
    }
  }
};

std::string CompilerOp::str() const {
  std::string out;
  out += StringPrintf("%s", OpUtil::name(code));
  if (HAS_ARG(code)) {
    out += StringPrintf(".%d", arg);
  }
  out += "[";
  out += StrUtil::join(regs.begin(), regs.end(), ",");
  out += "]";
  if (dead) {
    out += " DEAD ";
  }
  return out;
}

void CompilerState::dump(Writer* w) {
  for (BasicBlock* bb : bbs) {
    if (bb->dead) {
      continue;
    }

    w->printf("bb_%d: \n  ", bb->py_offset);
    w->write(StrUtil::join(bb->code, "\n  "));
    w->write(" -> ");
    w->write(StrUtil::join(bb->exits.begin(), bb->exits.end(), ",", [](BasicBlock* n) {
      return StringPrintf("bb_%d", n->py_offset);
    }));
    w->write("\n");
  }
}

std::string CompilerState::str() {
  StringWriter w;
  dump(&w);
  return w.str();
}

BasicBlock* CompilerState::alloc_bb(int offset, RegisterStack* entry_stack) {
  RegisterStack* entry_stack_copy = new RegisterStack(*entry_stack);
  BasicBlock* bb = new BasicBlock(offset, bbs.size(), entry_stack_copy);
  alloc_.push_back(bb);
  bbs.push_back(bb);
  this->bb_offsets[offset] = bb;
  return bb;
}

void CompilerState::remove_bb(BasicBlock* bb) {
  bbs.erase(std::find(bbs.begin(), bbs.end(), bb));
  this->bb_offsets.erase(this->bb_offsets.find(bb->py_offset));
}

std::string RegisterStack::str() {
  return StringPrintf("[%s]", StrUtil::join(regs.begin(), regs.end(), ",", &Coerce::t_str<int>).c_str());
}

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
  CompilerOp* op = _add_op(opcode, arg, 4);
  op->regs[0] = reg1;
  op->regs[1] = reg2;
  op->regs[2] = reg3;
  op->regs[3] = reg4;
  return op;
}

CompilerOp* BasicBlock::add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4, int reg5) {
  /* operation with 5 inputs and a destination register */
  CompilerOp* op = _add_op(opcode, arg, 4);
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


/*
 * The main event: convert from a stack machine to an infinite register machine.
 * We do this using a virtual stack.  Instead of opcodes pushing and popping
 * values from the * stack, we have them push and pop register names.  These
 * register names can then * be used to construct register versions of each
 * opcode.
 *
 * For example, the following operations translate to:
 *
 * LOAD_CONST 1
 * LOAD_CONST 2
 * ADD
 *
 * --->
 *
 * int r1 = 1 ('push' r1)
 * int r2 = 2 ('push' r2)
 * int r3 = add r1, r2 ('pop' r1, r2)
 */

BasicBlock* jump_prelude(CompilerState* state, RegisterStack *stack, int offset, BasicBlock* old) {
  // when revisiting a basic block, make sure we have the same registers
  // on the stack as whoever passed through here before

  BasicBlock* prelude = state->alloc_bb(-offset, stack);
  Reg_AssertEq(stack->regs.size(), old->entry_stack->regs.size());

  int n_moves = 0;
  for (size_t i = 0; i < stack->regs.size(); ++i) {
    int old_reg = old->entry_stack->regs[i];
    int curr_reg = stack->regs[i];
    if (old_reg != curr_reg) {
      // todo: if we ever change the interpreter to have a MOVE instruction
      // use that here
      prelude->add_dest_op(STORE_FAST, 0, curr_reg, old_reg);
      n_moves++;
    }
  }
  if (n_moves > 0) {
    //offset will get patched up later since we're adding 'old' to exits
    prelude->add_op(JUMP_ABSOLUTE, 0);
    prelude->exits.push_back(old);
    return prelude;
  } else {
    state->remove_bb(prelude);
    return old;
  }

}
BasicBlock* Compiler::registerize(CompilerState* state, RegisterStack *stack, int offset) {
  Py_ssize_t r;
  int oparg = 0;
  int opcode = 0;

  unsigned char* codestr = state->py_codestr;

  BasicBlock *last = NULL;
  BasicBlock *entry_point = NULL;

  auto iter = state->bb_offsets.find(offset);
  if (iter != state->bb_offsets.end()) {
    return jump_prelude(state, stack, offset, iter->second);
  }

  for (; offset < state->py_codelen; offset += CODESIZE(codestr[offset])) {
    opcode = codestr[offset];
    oparg = 0;
    if (HAS_ARG(opcode)) {
      oparg = GETARG(codestr, offset);
    }

    // Check if the opcode we've advanced to has already been generated.
    // If so, patch ourselves into it and return our entry point.
    iter = state->bb_offsets.find(offset);
    if (iter != state->bb_offsets.end()) {
      Reg_Assert(entry_point != NULL, "Bad entry point.");
      Reg_Assert(last != NULL, "Bad entry point.");
      BasicBlock* old = jump_prelude(state, stack, offset, iter->second);
      // If our previous block won't fall-through into this one, then
      // generate an explicit jump instruction.
      if (last->idx != old->idx - 1) {
        // The argument for jump absolute will be generated from the block exit information.
        last->add_op(JUMP_ABSOLUTE, 0);
      }
      last->exits.push_back(old);
      return entry_point;
    }

    BasicBlock *bb = state->alloc_bb(offset, stack);
    if (!entry_point) {
      entry_point = bb;
    }

    if (getenv("COMPILE_LOG")) {
      const char* name = NULL;
      if (oparg < PyTuple_Size(state->names)) {
        name = PyString_AsString(PyTuple_GetItem(state->names, oparg));
      }
      COMPILE_LOG("%5d: %s(%d) [%s] %s",
               offset, OpUtil::name(opcode), oparg, name, stack->str().c_str());
    }

    if (last) {
      last->exits.push_back(bb);
    }

    last = bb;
    switch (opcode) {
    // Stack pushing/popping

    case NOP:
      break;
    case ROT_TWO: {
      int r1 = stack->pop_register();
      int r2 = stack->pop_register();
      stack->push_register(r1);
      stack->push_register(r2);
      break;
    }
    case ROT_THREE: {
      int r1 = stack->pop_register();
      int r2 = stack->pop_register();
      int r3 = stack->pop_register();
      stack->push_register(r1);
      stack->push_register(r3);
      stack->push_register(r2);
      break;
    }
    case POP_TOP: {
      stack->pop_register();
      break;
    }
    case DUP_TOP: {
      int r1 = stack->pop_register();
      stack->push_register(r1);
      stack->push_register(r1);
      break;
    }
    case DUP_TOPX: {
      if (oparg == 2) {
        int r1 = stack->pop_register();
        int r2 = stack->pop_register();
        stack->push_register(r1);
        stack->push_register(r2);
        stack->push_register(r1);
        stack->push_register(r2);
      } else {
        int r1 = stack->pop_register();
        int r2 = stack->pop_register();
        int r3 = stack->pop_register();
        stack->push_register(r3);
        stack->push_register(r2);
        stack->push_register(r1);
        stack->push_register(r3);
        stack->push_register(r2);
        stack->push_register(r1);
      }
      break;
    }
      // Load operations: push one register onto the stack.
    case LOAD_CONST: {
      int r1 = oparg;
      int r2 = stack->push_register(state->num_reg++);
      bb->add_dest_op(LOAD_FAST, 0, r1, r2);
      break;
    }
    case LOAD_FAST: {
      int r1 = state->num_consts + oparg;
      int r2 = stack->push_register(state->num_reg++);
      bb->add_dest_op(LOAD_FAST, 0, r1, r2);
      break;
    }
    case LOAD_CLOSURE:
    case LOAD_DEREF:
    case LOAD_GLOBAL:
    case LOAD_LOCALS:
    case LOAD_NAME: {
      int r1 = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1);
      break;
    }
    case LOAD_ATTR: {
      int r1 = stack->pop_register();
      int r2 = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1, r2);
      break;
    }
    case STORE_FAST: {
      int r1 = stack->pop_register();
      // Decrement the old value.
      bb->add_dest_op(opcode, 0, r1, state->num_consts + oparg);
      break;
    }
      // Store operations remove one or more registers from the stack.
    case STORE_DEREF:
    case STORE_GLOBAL:
    case STORE_NAME: {
      int r1 = stack->pop_register();
      bb->add_op(opcode, oparg, r1);
      break;
    }

    case DELETE_GLOBAL: {
      bb->add_op(opcode, oparg);
      break;
    }

    case STORE_ATTR: {
      int r1 = stack->pop_register();
      int r2 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2);
      break;
    }
    case STORE_MAP: {
      int r1 = stack->pop_register();
      int r2 = stack->pop_register();
      int r3 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2, r3);
      stack->push_register(r3);
      break;
    }
    case STORE_SUBSCR: {
      int r1 = stack->pop_register();
      int r2 = stack->pop_register();
      int r3 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2, r3);
      break;
    }
    case GET_ITER: {
      int r1 = stack->pop_register();
      int r2 = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1, r2);
      break;
    }
    case SLICE + 0:
    case SLICE + 1:
    case SLICE + 2:
    case SLICE + 3: {
      int list, left, right;
      left = right = -1;
      if ((opcode - SLICE) & 2) right = stack->pop_register();
      if ((opcode - SLICE) & 1) left = stack->pop_register();
      list = stack->pop_register();
      int dst = stack->push_register(state->num_reg++);
      bb->add_dest_op(SLICE, 0, list, left, right, dst);
      break;
    }
    case STORE_SLICE + 0:
    case STORE_SLICE + 1:
    case STORE_SLICE + 2:
    case STORE_SLICE + 3: {
      int list, left, right, value;
      left = right = -1;
      if ((opcode - STORE_SLICE) & 2) right = stack->pop_register();
      if ((opcode - STORE_SLICE) & 1) left = stack->pop_register();
      list = stack->pop_register();
      value = stack->pop_register();
      bb->add_dest_op(STORE_SLICE, 0, list, left, right, value);
      break;
    }
    case DELETE_SLICE + 0:
    case DELETE_SLICE + 1:
    case DELETE_SLICE + 2:
    case DELETE_SLICE + 3: {
      int list, left, right;
      left = right = -1;
      if ((opcode - DELETE_SLICE) & 2) right = stack->pop_register();
      if ((opcode - DELETE_SLICE) & 1) left = stack->pop_register();
      list = stack->pop_register();
      bb->add_dest_op(DELETE_SLICE, 0, list, left, right);
      break;
    }
    case LIST_APPEND: {
      int item = stack->pop_register();
      int list = stack->peek_register(oparg);
      // Log_Info("%d %d", item, list);
      bb->add_op(opcode, 0, list, item);
      break;
    }
      // Unary operations: pop 1, push 1.
    case UNARY_NOT:
    case UNARY_POSITIVE:
    case UNARY_NEGATIVE:
    case UNARY_CONVERT:
    case UNARY_INVERT: {
      // Unary operations: pop 1, push 1.
      int r1 = stack->pop_register();
      int r2 = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1, r2);
      break;
    }
    case BINARY_POWER:
    case BINARY_MULTIPLY:
    case BINARY_DIVIDE:
    case BINARY_TRUE_DIVIDE:
    case BINARY_FLOOR_DIVIDE:
    case BINARY_MODULO:
    case BINARY_ADD:
    case BINARY_SUBTRACT:
    case BINARY_SUBSCR:
    case BINARY_LSHIFT:
    case BINARY_RSHIFT:
    case BINARY_AND:
    case BINARY_XOR:
    case BINARY_OR:
    case INPLACE_POWER:
    case INPLACE_MULTIPLY:
    case INPLACE_DIVIDE:
    case INPLACE_TRUE_DIVIDE:
    case INPLACE_FLOOR_DIVIDE:
    case INPLACE_MODULO:
    case INPLACE_ADD:
    case INPLACE_SUBTRACT:
    case INPLACE_LSHIFT:
    case INPLACE_RSHIFT:
    case INPLACE_AND:
    case INPLACE_XOR:
    case INPLACE_OR:
    case COMPARE_OP: {
      // Binary operations: pop 2, push 1.
      int r1 = stack->pop_register();
      int r2 = stack->pop_register();
      int r3 = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r2, r1, r3);
      break;
    }
    case CALL_FUNCTION:
    case CALL_FUNCTION_VAR:
    case CALL_FUNCTION_KW:
    case CALL_FUNCTION_VAR_KW: {
      int na = oparg & 0xff;
      int nk = (oparg >> 8) & 0xff;
      int n = na + 2 * nk;
      CompilerOp* f = bb->add_varargs_op(opcode, oparg, n + 2);
      for (r = n - 1; r >= 0; --r) {
        f->regs[r] = stack->pop_register();
      }
      f->regs[n] = stack->pop_register();
      f->regs[n + 1] = stack->push_register(state->num_reg++);
      Reg_AssertEq(f->arg, oparg);
      break;
    }
    case PRINT_ITEM: {
      int r1 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, -1);
      break;
    }
    case PRINT_ITEM_TO: {
      int r1 = stack->pop_register();
      int r2 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2);
      break;
    }
    case PRINT_NEWLINE_TO: {
      int r1 = stack->pop_register();
      bb->add_op(opcode, oparg, r1);
      break;
    }
    case PRINT_NEWLINE: {
      bb->add_op(opcode, oparg, -1);
      break;
    }
    case IMPORT_STAR: {
      int module = stack->pop_register();
      bb->add_op(opcode, oparg, module);
      break;
    }
    case IMPORT_FROM: {
      int module = stack->peek_register(1);
      int tgt = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, module, tgt);
      break;
    }
    case IMPORT_NAME: {
      int r1 = stack->pop_register();
      int r2 = stack->pop_register();
      int tgt = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1, r2, tgt);
      break;
    }
    case MAKE_FUNCTION: {
      int code = stack->pop_register();
      CompilerOp* op = bb->add_varargs_op(opcode, oparg, oparg + 2);
      op->regs[0] = code;
      for (int i = 0; i < oparg; ++i) {
        op->regs[i + 1] = stack->pop_register();
      }
      op->regs[oparg + 1] = stack->push_register(state->num_reg++);
      break;
    }
    case MAKE_CLOSURE: {
      int code = stack->pop_register();
      int closure_values = stack->pop_register();
      CompilerOp* op = bb->add_varargs_op(opcode, oparg, oparg + 3);
      op->regs[0] = code;
      op->regs[1] = closure_values;
      for (int i = 0; i < oparg; ++i) {
        op->regs[i + 2] = stack->pop_register();
      }
      op->regs[oparg + 2] = stack->push_register(state->num_reg++);
      break;
    }

    case BUILD_LIST:
    case BUILD_SET:
    case BUILD_TUPLE: {
      CompilerOp* f = bb->add_varargs_op(opcode, oparg, oparg + 1);
      for (r = oparg - 1; r >= 0; --r) {
        f->regs[r] = stack->pop_register();
      }
      f->regs[oparg] = stack->push_register(state->num_reg++);
      break;
    }
    case BUILD_MAP:
      bb->add_dest_op(BUILD_MAP, oparg, stack->push_register(state->num_reg++));
      break;
    case BUILD_CLASS: {
      int r1 = stack->pop_register();
      int r2 = stack->pop_register();
      int r3 = stack->pop_register();
      int tgt = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1, r2, r3, tgt);
      break;
    }
    case BUILD_SLICE: {
      int r1, r2, r3;
      if (oparg == 3) {
        r1 = stack->pop_register();
      } else {
        r1 = -1;
      }
      r2 = stack->pop_register();
      r3 = stack->pop_register();

      int dst = stack->push_register(state->num_reg++);
      COMPILE_LOG("BUILD_SLICE %d %d %d %d", r1, r2, r3, dst);
      bb->add_dest_op(opcode, 0, r1, r2, r3, dst);
      break;
    }
    case UNPACK_SEQUENCE: {
      int seq = stack->pop_register();

      for (r = oparg; r >= 1; --r) {
        int elt = stack->push_register(state->num_reg++);
        bb->add_dest_op(CONST_INDEX, r - 1, seq, elt);
      }
      break;
    }
//        case SETUP_EXCEPT:
//        case SETUP_FINALLY:
    case SETUP_LOOP: {
      stack->push_frame(offset + CODESIZE(state->py_codestr[offset]) + oparg);
      break;
    }
    case POP_BLOCK: {
      stack->pop_frame();
      // bb->add_op(opcode, oparg);
      break;
    }
    case RAISE_VARARGS: {
      int r1, r2, r3;
      r1 = r2 = r3 = -1;
      CompilerOp* op = bb->add_varargs_op(opcode, 0, oparg);
      if (oparg == 3) {
        r1 = stack->pop_register();
        r2 = stack->pop_register();
        r3 = stack->pop_register();
        op->regs.push_back(r1);
        op->regs.push_back(r2);
        op->regs.push_back(r3);
      } else if (oparg == 2) {
        r1 = stack->pop_register();
        r2 = stack->pop_register();
        op->regs.push_back(r1);
        op->regs.push_back(r2);
      } else if (oparg == 1) {
        r1 = stack->pop_register();
        op->regs.push_back(r1);
      }

      break;
    }
      // Control flow instructions - recurse down each branch with a copy of the current stack.
    case BREAK_LOOP: {
      Frame f = stack->pop_frame();
      COMPILE_LOG("Break loop -- jumping to %d", f.target);
      bb->add_op(opcode, 0);
      bb->exits.push_back(registerize(state, stack, f.target));
      return entry_point;
    }
    case CONTINUE_LOOP: {
      stack->pop_frame();
      bb->add_op(opcode, oparg);
      bb->exits.push_back(registerize(state, stack, oparg));
      return entry_point;
    }
    case FOR_ITER: {
      int r1 = stack->pop_register();
      RegisterStack a(*stack);
      RegisterStack b(*stack);
      a.push_register(r1);
      int r2 = a.push_register(state->num_reg++);

      bb->add_dest_op(opcode, 0, r1, r2);

      // fall-through if iterator had an item, jump forward if iterator is empty.
      BasicBlock* left = registerize(state, &a, offset + CODESIZE(opcode));
      BasicBlock* right = registerize(state, &b, offset + CODESIZE(opcode) + oparg);
      bb->exits.push_back(left);
      bb->exits.push_back(right);
      return entry_point;
    }
    case JUMP_IF_FALSE_OR_POP:
    case JUMP_IF_TRUE_OR_POP: {
      RegisterStack a(*stack);
      int r1 = stack->pop_register();
      RegisterStack b(*stack);
      bb->add_op(opcode, oparg, r1);

      BasicBlock* right = registerize(state, &a, oparg);
      BasicBlock* left = registerize(state, &b, offset + CODESIZE(opcode));
      bb->exits.push_back(left);
      bb->exits.push_back(right);
      return entry_point;
    }
    case POP_JUMP_IF_FALSE:
    case POP_JUMP_IF_TRUE: {
      int r1 = stack->pop_register();
      RegisterStack a(*stack);
      RegisterStack b(*stack);
      bb->add_op(opcode, oparg, r1);
      BasicBlock* left = registerize(state, &a, offset + CODESIZE(opcode));
      BasicBlock* right = registerize(state, &b, oparg);
      bb->exits.push_back(left);
      bb->exits.push_back(right);
      return entry_point;
    }
    case JUMP_FORWARD: {
      int dst = oparg + offset + CODESIZE(opcode);
      bb->add_op(JUMP_ABSOLUTE, dst);
      assert(dst <= state->py_codelen);
      BasicBlock* exit = registerize(state, stack, dst);
      bb->exits.push_back(exit);
      return entry_point;
    }
    case JUMP_ABSOLUTE: {
      bb->add_op(JUMP_ABSOLUTE, oparg);
      BasicBlock* exit = registerize(state, stack, oparg);
      bb->exits.push_back(exit);
      return entry_point;
    }
    case RETURN_VALUE: {
      int r1 = stack->pop_register();
      bb->add_op(opcode, 0, r1);
      return entry_point;
    }
    case END_FINALLY:
    case YIELD_VALUE:
    default:
      throw RException(PyExc_SyntaxError, "Unknown opcode %s, arg = %d", OpUtil::name(opcode), oparg);
      break;
    }
  }
  return entry_point;
}

class CompilerPass {
protected:
public:
  virtual void visit_op(CompilerOp* op) {
  }

public:
  virtual void visit_bb(BasicBlock* bb) {
    size_t n_ops = bb->code.size();
    for (size_t i = 0; i < n_ops; ++i) {
      CompilerOp* op = bb->code[i];
      if (!op->dead) {
        this->visit_op(op);
      }
    }
  }

  virtual void visit_fn(CompilerState* fn) {
    size_t n_bbs = fn->bbs.size();
    for (size_t i = 0; i < n_bbs; ++i) {
      fn->bbs[i]->visited = false;
    }

    for (size_t i = 0; i < n_bbs; ++i) {
      BasicBlock* bb = fn->bbs[i];
      if (!bb->visited && !bb->dead) {
        this->visit_bb(bb);
        bb->visited = true;
      }
    }
  }

  void operator()(CompilerState* fn) {
    this->visit_fn(fn);
  }

  virtual ~CompilerPass() {
  }

};

class BackwardPass: public CompilerPass {
public:
  void visit_bb(BasicBlock* bb) {
    size_t n_ops = bb->code.size();
    for (size_t i = n_ops - 1; i-- > 0;) {
      CompilerOp* op = bb->code[i];
      if (!op->dead) {
        this->visit_op(op);
      }
    }
  }

  void visit_fn(CompilerState* fn) {
    size_t n_bbs = fn->bbs.size();
    for (size_t i = 0; i < n_bbs; ++i) {
      fn->bbs[i]->visited = false;
    }

    for (size_t i = n_bbs - 1; i-- > 0;) {
      BasicBlock* bb = fn->bbs[i];
      if (!bb->visited && !bb->dead) {
        this->visit_bb(bb);
        bb->visited = true;
      }
    }
  }

};

class SortedPass: public CompilerPass {
// visit basic blocks in topologically sorted order
private:
  bool all_preds_visited(BasicBlock* bb) {
    size_t n_entries = bb->entries.size();
    for (size_t i = 0; i < n_entries; ++i) {
      if (!bb->entries[i]->visited) {
        return false;
      }
    }
    return true;
  }
protected:
  bool in_cycle;
public:

  void visit_fn(CompilerState* fn) {
    size_t n_bbs = fn->bbs.size();
    for (size_t i = 0; i < n_bbs; ++i) {
      fn->bbs[i]->visited = false;
    }

    std::queue<BasicBlock*> ready;
    std::queue<BasicBlock*> waiting;

    ready.push(fn->bbs[0]);

    while (ready.size() > 0 || waiting.size() > 0) {
      BasicBlock* bb = NULL;
      if (ready.size() > 0) {
        bb = ready.front();
        ready.pop();
      } else {
        bb = waiting.front();
        waiting.pop();
      }
      if (!bb->visited) {
        bb->visited = true;
        this->in_cycle = !(this->all_preds_visited(bb));
        this->visit_bb(bb);
        int n_exits = bb->exits.size();
        for (int i = 0; i < n_exits; ++i) {
          BasicBlock* succ = bb->exits[i];
          if (this->all_preds_visited(succ)) {
            ready.push(succ);
          } else {
            waiting.push(succ);
          }
        }
      }
    }
  }

};

class MarkEntries: public CompilerPass {
public:
  void visit_bb(BasicBlock* bb) {
    for (size_t i = 0; i < bb->exits.size(); ++i) {
      BasicBlock* next = bb->exits[i];
      next->entries.push_back(bb);
    }
  }
};

class FuseBasicBlocks: public CompilerPass {
public:
  void visit_bb(BasicBlock* bb) {
    if (bb->visited || bb->dead || bb->exits.size() != 1) {
      return;
    }

    BasicBlock* next = bb->exits[0];
    while (1) {
      if (next->entries.size() > 1 || next->visited) {
        break;
      }

      // Strip our branch instruction if we're being merged
      // into the following basic block.
      if (!bb->code.empty()) {
        CompilerOp* last = bb->code.back();
        if (OpUtil::is_branch(last->code)) {
          bb->code.pop_back();
        }
      }

      //        Log_Info("Merging %d into %d", next->idx, bb->idx);
      bb->code.insert(bb->code.end(), next->code.begin(), next->code.end());

      next->dead = next->visited = true;
      next->code.clear();
      bb->exits = next->exits;

      if (bb->exits.size() != 1) {
        break;
      }
      next = bb->exits[0];
    }

  }
};

class CopyPropagation: public CompilerPass {
public:
  void visit_bb(BasicBlock* bb) {
    std::map<int, int> env;
    size_t n_ops = bb->code.size();
    int source, target;
    for (size_t i = 0; i < n_ops; ++i) {
      CompilerOp * op = bb->code[i];

      // check all the registers and forward any that are in the env
      size_t n_inputs = op->num_inputs();
      for (size_t reg_idx = 0; reg_idx < n_inputs; reg_idx++) {
        auto iter = env.find(op->regs[reg_idx]);
        if (iter != env.end()) {
          op->regs[reg_idx] = iter->second;
        }
      }
      if (op->code == LOAD_FAST || op->code == STORE_FAST || op->code == LOAD_CONST) {
        source = op->regs[0];
        target = op->regs[1];
        auto iter = env.find(source);
        if (iter != env.end()) {
          source = iter->second;
        }
        env[target] = source;
      }
    }
  }
};

class UseCounts {
protected:
  std::map<int, int> counts;

  int get_count(int r) {
    std::map<int, int>::iterator iter = counts.find(r);
    return iter == counts.end() ? 0 : iter->second;
  }

  void incr_count(int r) {
    this->counts[r] = this->get_count(r) + 1;
  }

  void decr_count(int r) {
    this->counts[r] = this->get_count(r) - 1;
  }

  bool is_pure(int op_code) {
    Log_Debug("Checking if %s is pure\n", OpUtil::name(op_code));
    switch (op_code) {
    case LOAD_GLOBAL:
    case LOAD_FAST:
    case LOAD_DEREF:
    case LOAD_CLOSURE:
    case LOAD_LOCALS:
    case LOAD_CONST:
    case LOAD_NAME:
    case STORE_FAST:
    case STORE_DEREF:
    case BUILD_SLICE:
    case CONST_INDEX:
    case BUILD_TUPLE:
    case BUILD_LIST:
    case BUILD_SET:
    case BUILD_MAP:
    case MAKE_CLOSURE:
      return true;
    default:
      return false;
    }
  }
  void count_uses(CompilerState* fn) {
    size_t n_bbs = fn->bbs.size();
    for (size_t bb_idx = 0; bb_idx < n_bbs; ++bb_idx) {

      BasicBlock* bb = fn->bbs[bb_idx];
      size_t n_ops = bb->code.size();
      for (size_t op_idx = 0; op_idx < n_ops; ++op_idx) {
        CompilerOp* op = bb->code[op_idx];
        size_t n_inputs = op->num_inputs();

        if (n_inputs > 0) {
          for (size_t reg_idx = 0; reg_idx < n_inputs; reg_idx++) {
            this->incr_count(op->regs[reg_idx]);
          }
        }
      }
    }
  }
};

class StoreElim: public CompilerPass, UseCounts {
public:
  void visit_bb(BasicBlock* bb) {
    // map from registers to their last definition in the basic block
    std::map<int, CompilerOp*> env;

    // if we encounter a move X->Y when:
    //   - X is locally defined in the basic block
    //   - X is only used once (for this move)
    // then modify the defining instruction of X
    // to directly write to Y and mark the move X->Y as dead

    size_t n_ops = bb->code.size();
    int source, target;
    for (size_t i = 0; i < n_ops; ++i) {
      CompilerOp * op = bb->code[i];
      // check all the registers and forward any that are in the env
      size_t n_inputs = op->num_inputs();

      if (op->has_dest) {
        target = op->regs[n_inputs];
        env[target] = op;

        if (op->code == LOAD_FAST || op->code == STORE_FAST) {
          source = op->regs[0];
          auto iter = env.find(source);
          if (iter != env.end() && this->get_count(source) == 1) {
            CompilerOp* def = iter->second;
            def->regs[def->num_inputs()] = target;
            op->dead = true;
          }
        }
      }
    }
  }

  void visit_fn(CompilerState* fn) {
    this->count_uses(fn);
    CompilerPass::visit_fn(fn);
  }
};

class DeadCodeElim: public BackwardPass, UseCounts {
private:
public:
  void remove_dead_ops(BasicBlock* bb) {
    size_t live_pos = 0;
    size_t n_ops = bb->code.size();
    for (size_t i = 0; i < n_ops; ++i) {
      CompilerOp* op = bb->code[i];
      if (op->dead) {
        continue;
      }
      bb->code[live_pos++] = op;
    }

    bb->code.resize(live_pos);
  }

  void remove_dead_code(CompilerState* fn) {
    size_t i = 0;
    size_t live_pos = 0;
    size_t n_bbs = fn->bbs.size();
    for (i = 0; i < n_bbs; ++i) {
      BasicBlock* bb = fn->bbs[i];
      if (bb->dead) {
        continue;
      }

      this->remove_dead_ops(bb);
      fn->bbs[live_pos++] = bb;
    }
    fn->bbs.resize(live_pos);
  }

  void visit_op(CompilerOp* op) {

    size_t n_inputs = op->num_inputs();
    if ((n_inputs > 0) && (op->has_dest)) {
      int dest = op->regs[n_inputs];
      if (this->is_pure(op->code) && this->get_count(dest) == 0) {
        op->dead = true;
        // if an operation is marked dead, decrement the use counts
        // on all of its arguments
        for (size_t input_idx = 0; input_idx < n_inputs; ++input_idx) {
          this->decr_count(op->regs[input_idx]);
        }
      }
    }
  }

  void visit_fn(CompilerState* fn) {
    this->count_uses(fn);
    BackwardPass::visit_fn(fn);
    remove_dead_code(fn);
  }
};

class RenameRegisters: public CompilerPass {
  // simple renaming that ignore live ranges of registers
private:
  // Mapping from old -> new register names
  std::map<int, int> register_map_;

public:
  RenameRegisters() {
  }

  void visit_op(CompilerOp* op) {
    for (size_t i = 0; i < op->regs.size(); ++i) {
      int tgt;
      if (register_map_.find(op->regs[i]) == register_map_.end()) {
        Log_Fatal("No mapping for register: %s, [%d]", op->str().c_str(), op->regs[i]);
      }
      tgt = register_map_[op->regs[i]];
      op->regs[i] = tgt;
    }
  }

  void visit_fn(CompilerState* fn) {

    std::map<int, int> counts;

    for (BasicBlock* bb : fn->bbs) {
      if (bb->dead) continue;
      for (CompilerOp *op : bb->code) {
        if (op->dead) continue;
        for (int reg : op->regs) {
          ++counts[reg];
        }
      }
    }

    // A few fixed-register opcodes special case the invalid register.
    register_map_[-1] = -1;

    // Don't remap the const/local register aliases, even if we
    // don't see a usage point for them.
    for (int i = 0; i < fn->num_consts + fn->num_locals; ++i) {
      register_map_[i] = i;
    }

    int curr = fn->num_consts + fn->num_locals;
    for (int i = fn->num_consts + fn->num_locals; i < fn->num_reg; ++i) {
      if (counts[i] != 0) {
        register_map_[i] = curr++;
      }
    }

    int min_count = 0;
    for (int i = 0; i < fn->num_reg; ++i) {
      if (counts[i] != 0) {
        ++min_count;
      }
    }

    CompilerPass::visit_fn(fn);
    COMPILE_LOG("Register rename: keeping %d of %d registers (%d const+local, with arg+const folding: %d)",
             curr, fn->num_reg, fn->num_consts + fn->num_locals, min_count);
    fn->num_reg = curr;
  }
};

class CompactRegisters: public SortedPass, UseCounts {
private:
  // Mapping from old -> new register names
  std::map<int, int> register_map;
  std::stack<int> free_registers;
  int num_frozen;
  int max_register;

  std::set<int> bb_defs;
  bool defined_locally(int r) {
    return bb_defs.find(r) != bb_defs.end();
  }

public:

  void visit_op(CompilerOp* op) {

    size_t n_regs = op->regs.size();
    size_t n_input_regs;
    if (op->has_dest) {
      n_input_regs = n_regs - 1;
    } else {
      n_input_regs = n_regs;
    }
    int old_reg;
    int new_reg;
    for (size_t i = 0; i < n_input_regs; ++i) {
      old_reg = op->regs[i];
      if (old_reg >= num_frozen) {
        new_reg = register_map[old_reg];
        if (new_reg != 0) {
          op->regs[i] = new_reg;
          this->decr_count(old_reg);
          if (this->get_count(old_reg) == 0) {
            if (!this->in_cycle || this->defined_locally(old_reg)) {
              this->free_registers.push(new_reg);
            }
          }
        }
      }
    }
    if (op->has_dest) {
      old_reg = op->regs[n_input_regs];
      if (old_reg >= 0) {
        bb_defs.insert(old_reg);
        if (register_map.find(old_reg) != register_map.end()) {
          new_reg = register_map[old_reg];
        } else if (free_registers.size() > 0) {
          new_reg = free_registers.top();
          free_registers.pop();
          register_map[old_reg] = new_reg;
        } else {
          new_reg = max_register;
          register_map[old_reg] = new_reg;
          max_register++;
        }
        op->regs[n_input_regs] = new_reg;
      }
    }
  }

  void visit_bb(BasicBlock* bb) {
    this->bb_defs.clear();
    SortedPass::visit_bb(bb);

  }
  void visit_fn(CompilerState* fn) {

    this->count_uses(fn);

    // don't rename inputs, locals, or constants
    num_frozen = fn->num_locals + fn->num_consts;
    max_register = num_frozen;
    for (int i = 0; i < max_register; ++i) {
      register_map[i] = i;
    }
    SortedPass::visit_fn(fn);
  }
};

enum StaticType {
  UNKNOWN,
  INT,
  FLOAT,
  LIST,
  TUPLE,
  DICT,
  OBJ
};

class TypeInference {
protected:
  std::map<int, StaticType> types;

  StaticType get_type(int r) {
    std::map<int, StaticType>::iterator iter = this->types.find(r);
    if (iter == this->types.end()) {
      return UNKNOWN;
    } else {
      return iter->second;
    }
  }

  void update_type(int r, StaticType t) {
    StaticType old_t = this->get_type(r);
    if (old_t == UNKNOWN) {
      this->types[r] = t;
    } else if (old_t != t && old_t != OBJ) {
      this->types[r] = OBJ;
    }
  }

public:
  void infer(CompilerState* fn) {
    size_t n_bbs = fn->bbs.size();
    for (size_t bb_idx = 0; bb_idx < n_bbs; ++bb_idx) {

      BasicBlock* bb = fn->bbs[bb_idx];
      size_t n_ops = bb->code.size();
      for (size_t op_idx = 0; op_idx < n_ops; ++op_idx) {
        CompilerOp* op = bb->code[op_idx];
        size_t n_inputs = op->num_inputs();
        if (op->has_dest) {
          int dest = op->regs[n_inputs];
          StaticType t = OBJ;
          switch (op->code) {
          case BUILD_LIST:
            t = LIST;
            break;
          case BUILD_TUPLE:
            t = TUPLE;
            break;
          case BUILD_MAP:
            t = DICT;
            break;
          case LOAD_FAST:
            // todo: constant integers and floats
            break;
          }
          this->update_type(dest, t);
        }
      }
    }
  }
};

enum KnownMethod {
  METHOD_LIST_APPEND
};

class LocalTypeSpecialization: public CompilerPass, public TypeInference {
private:
  std::map<int, KnownMethod> known_methods;
  std::map<int, int> known_bound_objects;

  PyObject* names;
public:
  void visit_op(CompilerOp* op) {
    switch (op->code) {
    case LOAD_ATTR: {

      PyObject* attr_name_obj = PyTuple_GetItem(this->names, op->arg);
      char* attr_name = PyString_AsString(attr_name_obj);
      if (strcmp(attr_name, "append") == 0) {
        this->known_methods[op->regs[1]] = METHOD_LIST_APPEND;
        this->known_bound_objects[op->regs[1]] = op->regs[0];
      }
    }
      break;

    case CALL_FUNCTION: {
      int fn_reg = op->regs[op->num_inputs() - 1];
      auto iter = this->known_methods.find(fn_reg);
      if (iter != this->known_methods.end()) {
        switch (iter->second) {
        case METHOD_LIST_APPEND: {
          op->code = LIST_APPEND;

          int item = op->regs[0];
          int fn = op->regs[1];
          op->arg = 0;
          op->regs.clear();

          op->regs.push_back(this->known_bound_objects[fn]);
          op->regs.push_back(item);
          break;
        }
        }
      }
    }
      break;
    }
  }

  void visit_fn(CompilerState* fn) {
    this->infer(fn);
    this->names = fn->names;
    CompilerPass::visit_fn(fn);

  }
};

void optimize(CompilerState* fn) {
  MarkEntries()(fn);
  FuseBasicBlocks()(fn);

  if (!getenv("DISABLE_OPT")) {
    if (!getenv("DISABLE_COPY")) CopyPropagation()(fn);
    if (!getenv("DISABLE_STORE")) StoreElim()(fn);
    if (!getenv("DISABLE_TYPE_INFERENCE")) LocalTypeSpecialization()(fn);
  }

  DeadCodeElim()(fn);

  if (!getenv("DISABLE_OPT") && !getenv("DISABLE_COMPACT")) {
    CompactRegisters()(fn);
  }

  RenameRegisters()(fn);
  COMPILE_LOG(fn->str().c_str());
}

void lower_register_code(CompilerState* state, std::string *out) {

// first, dump all of the operations to the output buffer and record
// their positions.
  for (size_t i = 0; i < state->bbs.size(); ++i) {
    BasicBlock* bb = state->bbs[i];
    assert(!bb->dead);
    bb->reg_offset = out->size();
    for (size_t j = 0; j < bb->code.size(); ++j) {
      CompilerOp* c = bb->code[j];
      assert(!c->dead);

      size_t offset = out->size();
      out->resize(out->size() + RCompilerUtil::op_size(c));
      RCompilerUtil::lower_op(&(*out)[0] + offset, c);
      Log_Debug("Wrote op at offset %d, size: %d, %s", offset, RCompilerUtil::op_size(c), c->str().c_str());
    }
  }

// now patchup labels in the emitted code to point to the correct
// locations.
  int pos = 0;
  for (size_t i = 0; i < state->bbs.size(); ++i) {
    BasicBlock* bb = state->bbs[i];
    OpHeader* op = NULL;

    if (bb->code.empty()) {
      continue;
    }

    // Skip to the end of the basic block.
    for (size_t j = 0; j < bb->code.size(); ++j) {
      op = (OpHeader*) (out->data() + pos);
      Log_Debug("Checking op %s at offset %d.", OpUtil::name(op->code), pos);

      Reg_AssertEq(op->code, bb->code[j]->code);
      if (OpUtil::has_arg(op->code)) {
        Reg_AssertEq(op->arg, bb->code[j]->arg);
      } else {
        Reg_Assert(op->arg == 0 || OpUtil::is_branch(op->code), "Argument to non-argument op: %s, %d",
                   OpUtil::name(op->code), op->arg);
      }
      pos += RCompilerUtil::op_size(bb->code[j]);
    }

    Reg_Assert(op->code == RETURN_VALUE || OpUtil::is_branch(op->code) || (bb->exits[0] == state->bbs[i + 1]),
               "Non-local jump from non-branch op %s", OpUtil::name(op->code));

    if (OpUtil::is_branch(op->code) && op->code != RETURN_VALUE) {
      if (bb->exits.size() == 1) {
        BasicBlock& jmp = *bb->exits[0];
        ((BranchOp*) op)->label = jmp.reg_offset;
        Reg_AssertGt(jmp.reg_offset, 0);
        Reg_AssertEq(((BranchOp*)op)->label, jmp.reg_offset);
      } else {
        // One exit is the fall-through to the next block.
        BasicBlock& a = *bb->exits[0];
        BasicBlock& b = *bb->exits[1];
        BasicBlock& fallthrough = *state->bbs[i + 1];
        Reg_Assert(fallthrough.idx == a.idx || fallthrough.idx == b.idx, "One branch must fall-through (%d, %d) != %d",
                   a.idx, b.idx, fallthrough.idx);
        BasicBlock& jmp = (a.idx == fallthrough.idx) ? b : a;
//        Log_Info("%d, %d", a.idx, b.idx);
        Reg_AssertGt(jmp.reg_offset, 0);
        ((BranchOp*) op)->label = jmp.reg_offset;
        Reg_AssertEq(((BranchOp*)op)->label, jmp.reg_offset);
      }
    }
  }
}

RegisterCode* Compiler::compile_(PyObject* func) {
  PyCodeObject* code = NULL;
  if (PyFunction_Check(func)) {
    code = (PyCodeObject*) PyFunction_GET_CODE(func);
  } else if (PyCode_Check(func)) {
    code = (PyCodeObject*) func;
  } else {
    throw RException(PyExc_SystemError, "Not a function: %s", obj_to_str(func));
  }

  if (!code) {
    throw RException(PyExc_SystemError, "No code in function object.");
  }

  CompilerState state(code);
  RegisterStack stack;

  BasicBlock* entry_point = registerize(&state, &stack, 0);
  if (entry_point == NULL) {
    throw RException(PyExc_SystemError, "Failed to registerize %s", PyEval_GetFuncName(func));
  }

  optimize(&state);

  RegisterCode *regcode = new RegisterCode;

  lower_register_code(&state, &regcode->instructions);

  regcode->code_ = (PyObject*) code;
  regcode->version = 1;
  if (PyFunction_Check(func)) {
    regcode->function = func;
  } else {
    regcode->function = NULL;
  }
  regcode->mapped_registers = 0;
  regcode->mapped_labels = 0;
  regcode->num_registers = state.num_reg;

  regcode->num_freevars = PyTuple_GET_SIZE(code->co_freevars);
  regcode->num_cellvars = PyTuple_GET_SIZE(code->co_cellvars);
  regcode->num_cells = regcode->num_freevars + regcode->num_cellvars;

  return regcode;
}

