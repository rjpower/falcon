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
#include <vector>
#include <string>

#define GETARG(arr, i) ((int)((arr[i+2]<<8) + arr[i+1]))
#define CODESIZE(op)  (HAS_ARG(op) ? 3 : 1)

using namespace std;

struct RCompilerUtil {
  static int op_size(CompilerOp* op) {
    if (OpUtil::is_varargs(op->code)) {
      return sizeof(VarRegOp) + sizeof(Register) * op->regs.size();
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
        op->regs[i] = src->regs[i];

        // Guard against overflowing our register size.
        Reg_AssertEq(op->regs[i], src->regs[i]);
      }
      Reg_AssertEq(op->num_registers, src->regs.size());
    } else if (OpUtil::is_branch(src->code)) {
      BranchOp* op = (BranchOp*) dst;
      assert(src->regs.size() < 3);
      op->reg[0] = src->regs.size() > 0 ? src->regs[0] : -1;
      op->reg[1] = src->regs.size() > 1 ? src->regs[1] : -1;

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
      op->hint = kInvalidHint;
    }
  }
};

std::string CompilerOp::str() const {
  std::string out;
  out += StringPrintf("%s ", OpUtil::name(code));
  if (HAS_ARG(code)) {
    out += StringPrintf("(%d) ", arg);
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

    w->printf("bb_%d: \n  ", bb->idx);
    w->write(StrUtil::join(bb->code, "\n  "));
    w->write(" -> ");
    w->write(StrUtil::join(bb->exits.begin(), bb->exits.end(), ",", [](BasicBlock* n) {
      return StringPrintf("bb_%d", n->idx);
    }));
    w->write("\n");
  }
}

std::string CompilerState::str() {
  StringWriter w;
  dump(&w);
  return w.str();
}

BasicBlock* CompilerState::alloc_bb(int offset) {
  BasicBlock* bb = new BasicBlock(offset, bbs.size());
  alloc_.push_back(bb);
  bbs.push_back(bb);
  return bb;
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

BasicBlock::BasicBlock(int offset, int idx) {
  reg_offset = 0;
  py_offset = offset;
  visited = 0;
  dead = false;
  this->idx = idx;
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
  Reg_AssertNe(reg, kInvalidRegister);
  regs.push_back(reg);
  return reg;
}

int RegisterStack::pop_register() {
  Reg_AssertGt((int)regs.size(), 0);
  int reg = regs.back();
  Reg_AssertNe(reg, kInvalidRegister);
  regs.pop_back();
  return reg;
}

int RegisterStack::peek_register(int offset) {
  return regs[regs.size() - offset - 1];
}

void copy_stack(RegisterStack *from, RegisterStack* to) {
  to->frames = from->frames;
  to->regs = from->regs;
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
BasicBlock* Compiler::registerize(CompilerState* state, RegisterStack *stack, int offset) {
  Py_ssize_t r;
  int oparg = 0;
  int opcode = 0;

  unsigned char* codestr = state->py_codestr;

  BasicBlock *last = NULL;
  BasicBlock *entry_point = NULL;

  // If we've already visited this opcode, return the previous block for it.
  for (size_t i = 0; i < state->bbs.size(); ++i) {
    BasicBlock *old = state->bbs[i];
    if (old->py_offset == offset) {
      return old;
    }
  }

  for (; offset < state->py_codelen; offset += CODESIZE(codestr[offset])) {
    opcode = codestr[offset];
    oparg = 0;
    if (HAS_ARG(opcode)) {
      oparg = GETARG(codestr, offset);
    }

    const char* name = NULL;
    if (oparg < PyTuple_Size(state->names)) {
      name = PyString_AsString(PyTuple_GetItem(state->names, oparg));
    }
    Log_Info("%5d: %s(%d) [%s] %s", offset, OpUtil::name(opcode), oparg, name, stack->str().c_str());
    // Check if the opcode we've advanced to has already been generated.
    // If so, patch ourselves into it and return our entry point.
    for (size_t i = 0; i < state->bbs.size(); ++i) {
      BasicBlock *old = state->bbs[i];
      if (old->py_offset == offset) {
        Reg_Assert(entry_point != NULL, "Bad entry point.");
        Reg_Assert(last != NULL, "Bad entry point.");
        last->exits.push_back(old);
        return entry_point;
      }
    }

    BasicBlock *bb = state->alloc_bb(offset);
    if (!entry_point) {
      entry_point = bb;
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
//      bb->add_op(DECREF, 0, r1);
      break;
    }
    case DUP_TOP: {
      int r1 = stack->pop_register();
      stack->push_register(r1);
      stack->push_register(r1);
      //bb->add_op(INCREF, 0, r1);
      break;
    }
    case DUP_TOPX: {
      if (oparg == 2) {
        int r1 = stack->pop_register();
        int r2 = stack->pop_register();
        //bb->add_op(INCREF, 0, r1);
        //bb->add_op(INCREF, 0, r2);
        stack->push_register(r1);
        stack->push_register(r2);
        stack->push_register(r1);
        stack->push_register(r2);
      } else {
        int r1 = stack->pop_register();
        int r2 = stack->pop_register();
        int r3 = stack->pop_register();
        //bb->add_op(INCREF, 0, r1);
        //bb->add_op(INCREF, 0, r2);
        //bb->add_op(INCREF, 0, r3);
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
//      bb->add_op(DECREF, 0, state->num_consts + oparg);
      bb->add_dest_op(opcode, 0, r1, state->num_consts + oparg);
      break;
    }
      // Store operations remove one or more registers from the stack.
    case STORE_DEREF:
    case STORE_GLOBAL:
    case STORE_NAME: {
      int r1 = stack->pop_register();
      bb->add_op(opcode, oparg, r1);
//      bb->add_op(DECREF, 0, r1);
      break;
    }
    case STORE_ATTR: {
      int r1 = stack->pop_register();
      int r2 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2);
//      bb->add_op(DECREF, 0, r1);
//      bb->add_op(DECREF, 0, r2);
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

//            for (r = n; r >= 0; --r) { bb->add_op(DECREF, 0, f->regs[r]); }
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
      int module = stack->peek_register(0);
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
      op->regs[0]  = code;
      for (int i = 0; i < oparg; ++i) {
        op->regs[i + 1] = stack->pop_register();
      }
      op->regs[oparg + 1] = stack->push_register(state->num_reg++);
      break;
    }
    case BUILD_LIST:
    case BUILD_SET:
    case BUILD_TUPLE: {
      CompilerOp* f = bb->add_varargs_op(opcode, oparg, oparg + 1);
      for (r = 0; r < oparg; ++r) {
        f->regs[r] = stack->pop_register();
      }
      f->regs[oparg] = stack->push_register(state->num_reg++);
      break;
    }
    case BUILD_CLASS: {
      int r1 = stack->pop_register();
      int r2 = stack->pop_register();
      int r3 = stack->pop_register();
      int tgt = stack->push_register(state->num_reg++);
      bb->add_dest_op(opcode, oparg, r1, r2, r3, tgt);
      break;
    }
    case UNPACK_SEQUENCE: {
      Register seq = stack->pop_register();

      for (r = oparg; r >= 1; --r) {
        Register elt = stack->push_register(state->num_reg++);
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
      if (oparg == 3) {
        r1 = stack->pop_register();
        r2 = stack->pop_register();
        r3 = stack->pop_register();
      } else if (oparg == 2) {
        r1 = stack->pop_register();
        r2 = stack->pop_register();
      } else if (oparg == 1) {
        r1 = stack->pop_register();
      }
      bb->add_op(opcode, 0, r1, r2, r3);
      break;
    }
      // Control flow instructions - recurse down each branch with a copy of the current stack.
    case BREAK_LOOP: {
      Frame f = stack->pop_frame();
      bb->add_op(opcode, oparg);
      bb->exits.push_back(registerize(state, stack, f.target));
      return entry_point;
    }
    case CONTINUE_LOOP: {
      stack->pop_frame();
      bb->add_op(opcode, oparg);
      bb->exits.push_back(registerize(state, stack, oparg));
      break;
    }
    case FOR_ITER: {
      RegisterStack a, b;
      int r1 = stack->pop_register();
      copy_stack(stack, &a);
      copy_stack(stack, &b);
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
      RegisterStack a, b;
      copy_stack(stack, &a);
      int r1 = stack->pop_register();
      copy_stack(stack, &b);
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
      RegisterStack a, b;
      copy_stack(stack, &a);
      copy_stack(stack, &b);
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
    std::map<Register, Register> env;
    size_t n_ops = bb->code.size();
    Register source, target;
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
  std::map<Register, int> counts;

  int get_count(Register r) {
    std::map<Register, int>::iterator iter = counts.find(r);
    return iter == counts.end() ? 0 : iter->second;
  }

  void incr_count(Register r) {
    this->counts[r] = this->get_count(r) + 1;
  }

  void decr_count(Register r) {
    this->counts[r] = this->get_count(r) - 1;
  }

  bool is_pure(int op_code) {
    Log_Debug("Checking if %s is pure\n", OpUtil::name(op_code));
    switch (op_code) {
    case LOAD_LOCALS:
    case LOAD_CONST:
    case LOAD_NAME:
    case BUILD_TUPLE:
    case BUILD_LIST:
    case BUILD_SET:
    case BUILD_MAP:
    case MAKE_CLOSURE:
    case LOAD_GLOBAL:
    case LOAD_FAST:
    case LOAD_DEREF:
    case LOAD_CLOSURE:
    case BUILD_SLICE:
    case CONST_INDEX:
    case STORE_FAST:
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
    std::map<Register, CompilerOp*> env;

    // if we encounter a move X->Y when:
    //   - X is locally defined in the basic block
    //   - X is only used once (for this move)
    // then modify the defining instruction of X
    // to directly write to Y and mark the move X->Y as dead

    size_t n_ops = bb->code.size();
    Register source, target;
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
    // printf("visit_op %s (code = %d)\n", op->str().c_str(), op->code);

    size_t n_inputs = op->num_inputs();
    // printf(" -- n_inputs %d\n", n_inputs);
    // printf(" -- has_dest %d\n", op->has_dest);
    // printf(" -- is pure? %d\n", this->is_pure(op->code));
    if ((n_inputs > 0) && (op->has_dest)) {
      Register dest = op->regs[n_inputs];
      // printf(" ** use count %d\n", this->get_count(dest));
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

class RenameRegisters: public CompilerPass, UseCounts {
private:
  // Mapping from old -> new register names
  google::dense_hash_map<int, int> register_map_;

public:
  RenameRegisters() {
    register_map_.set_empty_key(kInvalidRegister);
  }

  void visit_op(CompilerOp* op) {
    for (size_t i = 0; i < op->regs.size(); ++i) {
      int tgt;
      if (register_map_.find(op->regs[i]) == register_map_.end()) {
        tgt = kInvalidRegister;
        Log_Fatal("No mapping for register: %s, [%d]", op->str().c_str(), op->regs[i]);
      } else {
        tgt = register_map_[op->regs[i]];
      }
      op->regs[i] = tgt;
    }
  }

  void visit_fn(CompilerState* fn) {
    count_uses(fn);

    // Don't reuse the const/local register aliases.
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
    Log_Info("Register rename: keeping %d of %d registers (%d const+local, with arg+const folding: %d)",
             curr, fn->num_reg, fn->num_consts + fn->num_locals, min_count);
    fn->num_reg = curr;
  }
};

void optimize(CompilerState* fn) {
  MarkEntries()(fn);
  FuseBasicBlocks()(fn);
  CopyPropagation()(fn);
  StoreElim()(fn);
  DeadCodeElim()(fn);
//  RenameRegisters()(fn);

  Log_Info(fn->str().c_str());
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
  regcode->code_ = (PyObject*)code;
  regcode->version = 1;
  if (PyFunction_Check(func)) {
    regcode->function = func;
  } else {
    regcode->function = NULL;
  }
  regcode->mapped_registers = 0;
  regcode->mapped_labels = 0;
  regcode->num_registers = state.num_reg;

  return regcode;
}

