#include "py_include.h"
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


static int num_python_ops(const char* code, int len) {
  int pos = 0;
  int count = 0;
  while (pos < len) {
    pos += CODESIZE((uint8_t) code[pos]);
    ++count;

  }
  return count;
}

using namespace std;

struct RCompilerUtil {
  static int op_size(CompilerOp* op) {
    if (OpUtil::is_varargs(op->code)) {
      return sizeof(VarRegOp) + sizeof(RegisterOffset) * op->regs.size();
    } else if (OpUtil::is_branch(op->code)) {
      int n_regs = op->regs.size();
      if (n_regs == 0) {
        return sizeof(BranchOp<0> );
      } else if (n_regs == 1) {
        return sizeof(BranchOp<1> );
      } else {
        return sizeof(BranchOp<2> );
      }
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
      int n_regs = src->regs.size();
      Reg_AssertLe(n_regs, 2);
      if (n_regs == 2) {
        BranchOp<2>* op = (BranchOp<2>*) dst;
        op->reg[0] = src->regs[0];
        op->reg[1] = src->regs[1];
        op->label = 0;
      } else if (n_regs == 1) {
        BranchOp<1>* op = (BranchOp<1>*) dst;
        op->reg[0] = src->regs[0];
        op->label = 0;
      } else {
        BranchOp<0>* op = (BranchOp<0>*) dst;
        op->label = 0;
      }
    } else {
      Reg_AssertLe(src->regs.size(), 4ul);
      RegOp<0>* op = (RegOp<0>*) dst;
      for (size_t i = 0; i < src->regs.size(); ++i) {
//        op->reg.set(i, src->regs[i]);
        op->reg[i] = src->regs[i];
      }
#if GETATTR_HINTS
      op->hint_pos = kInvalidHint;
#endif
    }

  }
};




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

        stack->push_register(r2);
        stack->push_register(r1);
        stack->push_register(r2);
        stack->push_register(r1);

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
      stack->push_register(oparg);
      /*int r1 = oparg;
       int r2 = stack->push_register(state->num_reg++);
       bb->add_dest_op(LOAD_FAST, 0, r1, r2);
       */
      break;
    }
    case LOAD_FAST: {
      int r1 = state->num_consts + oparg;
      stack->push_register(r1);
      /*
       int r2 = stack->push_register(state->num_reg++);
       bb->add_dest_op(LOAD_FAST, 0, r1, r2);
       */
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
//      Log_Info("%d[%d:%d] = %d", list, left, right, value);
      bb->add_op(STORE_SLICE, 0, list, left, right, value);
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
    case CALL_FUNCTION: {
      int na = oparg & 0xff;
      int nk = (oparg >> 8) & 0xff;
      // positional + (key=value) keywords
      int n = na + 2 * nk;

      CompilerOp* f = bb->add_varargs_op(opcode, oparg, n + 2);

      // pop off the args and then function
      stack->fill_register_array(f->regs, n + 1);
      f->regs[n + 1] = stack->push_register(state->num_reg++);
      Reg_AssertEq(f->arg, oparg);
      break;
    }

    case CALL_FUNCTION_VAR: {
      int na = oparg & 0xff;
      int nk = (oparg >> 8) & 0xff;
      // positional + (key=value) keywords
      int n = na + 2 * nk;

      // nargs + function + result + varargs
      CompilerOp* f = bb->add_varargs_op(opcode, oparg, n + 3);
      // pop off the varargs tuple, the actual args, and the function
      stack->fill_register_array(f->regs, n + 2);
      f->regs[n + 1] = stack->push_register(state->num_reg++);
      Reg_AssertEq(f->arg, oparg);
      break;
    }

    case CALL_FUNCTION_KW: {
      int na = oparg & 0xff;
      int nk = (oparg >> 8) & 0xff;
      // args = positional + (key=value) keywords + kwargs dict
      int n = na + 2 * nk;

      // nargs + function + result + kwdict
      CompilerOp* f = bb->add_varargs_op(opcode, oparg, n + 3);

      // pop off the kwdict, the args, and the function
      stack->fill_register_array(f->regs, n + 2);
      f->regs[n + 2] = stack->push_register(state->num_reg++);
      Reg_AssertEq(f->arg, oparg);
      break;
    }
    case CALL_FUNCTION_VAR_KW: {

      int na = oparg & 0xff;
      int nk = (oparg >> 8) & 0xff;
      //  positional + (key=value) keywords
      int n = na + 2 * nk;

      // leave room for function + args + result + varargs + kwdict
      CompilerOp* f = bb->add_varargs_op(opcode, oparg, n + 4);
      // pop off the kwdict, varargs, arguments, and function
      stack->fill_register_array(f->regs, n + 3);
      f->regs[n + 3] = stack->push_register(state->num_reg++);
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

    case SETUP_EXCEPT:
    case SETUP_FINALLY:
    case END_FINALLY:
    case YIELD_VALUE:
    default:
      throw RException(PyExc_SyntaxError, "Unsupported opcode %s, arg = %d", OpUtil::name(opcode), oparg);
      break;
    }
  }
  return entry_point;
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
        Reg_Assert(op->arg == bb->code[j]->arg, "Malformed bytecode arg %d for %s",
                   bb->code[j]->arg, OpUtil::name(op->code));
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
        ((BranchOp<0>*) op)->label = jmp.reg_offset;
        Reg_AssertGt(jmp.reg_offset, 0);
        Reg_AssertEq(((BranchOp<0>*)op)->label, jmp.reg_offset);
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
        ((BranchOp<0>*) op)->label = jmp.reg_offset;
        Reg_AssertEq(((BranchOp<0>*)op)->label, jmp.reg_offset);
      }
    }
  }
}

#include "optimizations.h"

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

  Log_Info(
      "COMPILED %s, %d registers, %d operations, %d stack ops.",
      PyEval_GetFuncName(func), regcode->num_registers, state.num_ops(), num_python_ops(PyString_AsString(code->co_code), PyString_GET_SIZE(code->co_code)));

  return regcode;
}

