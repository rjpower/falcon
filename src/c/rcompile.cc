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

// While compiling, we use an expanded form to represent opcodes.  This
// is translated to a compact instruction stream as the last compilation
// step.
typedef struct {
  int code;
  int arg;

  // this instruction has been marked dead by an optimization pass,
  // and should be ignored.
  bool dead;

  std::vector<Register> regs;
} CompilerOp;

struct BasicBlock;

struct BasicBlock {
private:
  CompilerOp* _add_op(int opcode, int arg, int num_regs) {
    CompilerOp* op = new CompilerOp;
    op->code = opcode;
    op->regs.resize(num_regs);
    op->dead = 0;
    op->arg = arg;
    code.push_back(op);

    return op;
  }
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

  BasicBlock(int offset, int idx) {
    reg_offset = 0;
    py_offset = offset;
    visited = 0;
    dead = false;
    this->idx = idx;
  }

  CompilerOp* add_op(int opcode, int arg) {
    return _add_op(opcode, arg, 0);
  }

  CompilerOp* add_op(int opcode, int arg, int reg1) {
    CompilerOp* op = _add_op(opcode, arg, 1);
    op->regs[0] = reg1;
    return op;
  }

  CompilerOp* add_op(int opcode, int arg, int reg1, int reg2) {
    CompilerOp* op = _add_op(opcode, arg, 2);
    op->regs[0] = reg1;
    op->regs[1] = reg2;
    return op;
  }

  CompilerOp* add_op(int opcode, int arg, int reg1, int reg2, int reg3) {
    CompilerOp* op = _add_op(opcode, arg, 3);
    op->regs[0] = reg1;
    op->regs[1] = reg2;
    op->regs[2] = reg3;
    return op;
  }

  CompilerOp* add_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4) {
    CompilerOp* op = _add_op(opcode, arg, 4);
    op->regs[0] = reg1;
    op->regs[1] = reg2;
    op->regs[2] = reg3;
    op->regs[3] = reg4;
    return op;
  }

  CompilerOp* add_varargs_op(int opcode, int arg, int num_regs) {
    return _add_op(opcode, arg, num_regs);
  }
};

struct Frame {
  int target;
  int stack_pos;
};

struct RegisterStack {
  int regs[REG_MAX_STACK];
  int stack_pos;

  Frame frames[REG_MAX_FRAMES];
  int num_frames;

  RegisterStack() :
      stack_pos(-1), num_frames(0) {
  }

  void push_frame(int target) {
    assert(num_frames < REG_MAX_FRAMES);
    Frame* f = &frames[num_frames++];
    f->target = target;
    f->stack_pos = stack_pos;
  }

  Frame* pop_frame() {
    assert(num_frames > 0);
    Frame* f = &frames[--num_frames];
    stack_pos = f->stack_pos;
    return f;
  }

  int push_register(int reg) {
    // fprintf(stderr, "Pushing register %d, pos %d\n", reg, stack_pos + 1);
    assert(stack_pos < REG_MAX_STACK);
    regs[++stack_pos] = reg;
    return reg;
  }

  int pop_register() {
    assert(stack_pos >= 0);
    int reg = regs[stack_pos--];
    assert(reg >= -1);
    // fprintf(stderr, "Popped register %d, pos: %d\n", reg, stack_pos + 1);
    return reg;
  }

  int peek_register(int reg) {
    return regs[stack_pos - reg];
  }

  void print() {
    int i;
    fprintf(stderr, "[");
    for (i = 0; i <= stack_pos; ++i) {
      fprintf(stderr, "%d, ", regs[i]);
    }
    fprintf(stderr, "]\n");
  }

};

struct CompilerState {
  std::vector<BasicBlock*> bbs;

  unsigned char* py_codestr;
  Py_ssize_t py_codelen;

  int num_reg;
  int num_consts;
  int num_locals;

  BasicBlock* alloc_bb(int offset) {
    BasicBlock* bb = new BasicBlock(offset, bbs.size());
    bbs.push_back(bb);
    return bb;
  }
};

void copy_stack(RegisterStack *from, RegisterStack* to) {
  memcpy(to->regs, from->regs, sizeof(from->regs));
  to->stack_pos = from->stack_pos;
  memcpy(to->frames, from->frames, sizeof(from->frames));
  to->num_frames = from->num_frames;
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
 * r1 = 1 ('push' r1)
 * r2 = 2 ('push' r2)
 * r3 = add r1, r2 ('pop' r1, r2)
 */
static BasicBlock* registerize(CompilerState* state, RegisterStack *stack, int offset) {
  Py_ssize_t r;
  int r1, r2, r3, r4;
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

    /* The following routines only affect the register stack, and their
     * effect can be captured statically.  We therefore do not have to emit
     * an opcode for them.
     */
    switch (opcode) {
    case NOP:
      continue;
    case ROT_TWO:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      stack->push_register(r1);
      stack->push_register(r2);
      continue;
    case ROT_THREE:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      r3 = stack->pop_register();
      stack->push_register(r1);
      stack->push_register(r3);
      stack->push_register(r2);
      continue;
    default:
      break;
    }

    // Check if the opcode we've advanced to has already been generated.
    // If so, patch ourselves into it and return our entry point.
    for (size_t i = 0; i < state->bbs.size(); ++i) {
      BasicBlock *old = state->bbs[i];
      if (old->py_offset == offset) {
        assert(entry_point != NULL);
        assert(last != NULL);
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
    case POP_TOP:
      r1 = stack->pop_register();
      bb->add_op(DECREF, 0, r1);
      break;
    case DUP_TOP:
      r1 = stack->pop_register();
      stack->push_register(r1);
      stack->push_register(r1);
      bb->add_op(INCREF, 0, r1);
      break;
    case DUP_TOPX:
      if (oparg == 2) {
        r1 = stack->pop_register();
        r2 = stack->pop_register();
        bb->add_op(INCREF, 0, r1);
        bb->add_op(INCREF, 0, r2);
        stack->push_register(r1);
        stack->push_register(r2);
        stack->push_register(r1);
        stack->push_register(r2);
      } else {
        r1 = stack->pop_register();
        r2 = stack->pop_register();
        r3 = stack->pop_register();
        bb->add_op(INCREF, 0, r1);
        bb->add_op(INCREF, 0, r2);
        bb->add_op(INCREF, 0, r3);
        stack->push_register(r3);
        stack->push_register(r2);
        stack->push_register(r1);
        stack->push_register(r3);
        stack->push_register(r2);
        stack->push_register(r1);
      }
      break;
      // Load operations: push one register onto the stack.
    case LOAD_CONST:
      r1 = stack->push_register(oparg);
      bb->add_op(INCREF, 0, r1);
      break;
    case LOAD_FAST:
      r1 = stack->push_register(state->num_consts + oparg);
      bb->add_op(INCREF, 0, r1);
      break;
    case LOAD_CLOSURE:
    case LOAD_DEREF:
    case LOAD_GLOBAL:
    case LOAD_LOCALS:
    case LOAD_NAME:
      r1 = stack->push_register(state->num_reg++);
      bb->add_op(opcode, oparg, r1);
      bb->add_op(INCREF, 0, r1);
      break;
    case LOAD_ATTR:
      r1 = stack->pop_register();
      r2 = stack->push_register(state->num_reg++);
      bb->add_op(opcode, oparg, r1, r2);
      bb->add_op(DECREF, 0, r1);
      break;
    case STORE_FAST:
      r1 = stack->pop_register();
      // Decrement the old value.
      bb->add_op(DECREF, 0, state->num_consts + oparg);
      bb->add_op(opcode, 0, r1, state->num_consts + oparg);
      break;
      // Store operations remove one or more registers from the stack.
    case STORE_DEREF:
    case STORE_GLOBAL:
    case STORE_NAME:
      r1 = stack->pop_register();
      bb->add_op(opcode, oparg, r1);
      bb->add_op(DECREF, 0, r1);
      break;
    case STORE_ATTR:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2);
      bb->add_op(DECREF, 0, r1);
      bb->add_op(DECREF, 0, r2);
      break;
    case STORE_MAP:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      r3 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2, r3);
      stack->push_register(r3);
      bb->add_op(DECREF, 0, r1);
      bb->add_op(DECREF, 0, r2);
      break;
    case STORE_SUBSCR:
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      r3 = stack->pop_register();
      bb->add_op(opcode, oparg, r1, r2, r3);
      bb->add_op(DECREF, 0, r1);
      bb->add_op(DECREF, 0, r2);
      bb->add_op(DECREF, 0, r3);
      break;
    case GET_ITER:
      r1 = stack->pop_register();
      r2 = stack->push_register(state->num_reg++);
      bb->add_op(opcode, oparg, r1, r2);
      bb->add_op(DECREF, oparg, r1);
      break;
    case SLICE + 0:
    case SLICE + 1:
    case SLICE + 2:
    case SLICE + 3:
      r1 = r2 = r3 = r4 = -1;
      if ((opcode - SLICE) & 2)
        r3 = stack->pop_register();
      if ((opcode - SLICE) & 1)
        r2 = stack->pop_register();
      r1 = stack->pop_register();
      r4 = stack->push_register(state->num_reg++);

      if (r2 == -1) {
        bb->add_op(opcode, oparg, r1, r4);
      } else {
        if (r3 == -1) {
          bb->add_op(opcode, oparg, r1, r2, r4);
        } else {
          bb->add_op(opcode, oparg, r1, r2, r3, r4);
        }
      }
      break;
    case STORE_SLICE + 0:
    case STORE_SLICE + 1:
    case STORE_SLICE + 2:
    case STORE_SLICE + 3:
      r1 = r2 = r3 = r4 = -1;
      if ((opcode - STORE_SLICE) & 2)
        r4 = stack->pop_register();
      if ((opcode - STORE_SLICE) & 1)
        r3 = stack->pop_register();
      r2 = stack->pop_register();
      r1 = stack->pop_register();
      if (r3 == -1) {
        bb->add_op(opcode, oparg, r1, r2);
      } else {
        if (r4 == -1) {
          bb->add_op(opcode, oparg, r1, r2, r3);
        } else {
          bb->add_op(opcode, oparg, r1, r2, r3, r4);
        }
      }
      break;
    case DELETE_SLICE + 0:
    case DELETE_SLICE + 1:
    case DELETE_SLICE + 2:
    case DELETE_SLICE + 3:
      r1 = r2 = r3 = r4 = -1;
      if ((opcode - DELETE_SLICE) & 2)
        r4 = stack->pop_register();
      if ((opcode - DELETE_SLICE) & 1)
        r3 = stack->pop_register();
      r2 = stack->pop_register();
      r1 = stack->pop_register();
      if (r3 == -1) {
        bb->add_op(opcode, oparg, r1, r2);
      } else {
        if (r4 == -1) {
          bb->add_op(opcode, oparg, r1, r2, r3);
        } else {
          bb->add_op(opcode, oparg, r1, r2, r3, r4);
        }
      }
      break;
    case LIST_APPEND:
      r1 = stack->pop_register();
      r2 = stack->peek_register(oparg);
      bb->add_op(opcode, oparg, r1, r2);
      break;
    case UNARY_NOT:
      // Unary operations: pop 1, push 1.
      r1 = stack->pop_register();
      r2 = stack->push_register(state->num_reg++);
      bb->add_op(opcode, oparg, r1, r2);
      break;
    case UNARY_POSITIVE:
    case UNARY_NEGATIVE:
    case UNARY_CONVERT:
    case UNARY_INVERT:
      // Unary operations: pop 1, push 1.
      r1 = stack->pop_register();
      r2 = stack->push_register(state->num_reg++);
      bb->add_op(opcode, oparg, r1, r2);
      bb->add_op(DECREF, 0, r1);
      break;
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
    case COMPARE_OP:
      // Binary operations: pop 2, push 1.
      r1 = stack->pop_register();
      r2 = stack->pop_register();
      r3 = stack->push_register(state->num_reg++);
      bb->add_op(opcode, oparg, r1, r2, r3);
      bb->add_op(DECREF, 0, r1);
      bb->add_op(DECREF, 0, r2);
      break;
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
      assert(f->arg == oparg);

//            for (r = n; r >= 0; --r) { bb->add_op(DECREF, 0, f->regs[r]); }
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
    case UNPACK_SEQUENCE: {
      CompilerOp* f = bb->add_varargs_op(opcode, oparg, oparg + 1);
      f->regs[0] = stack->pop_register();
      for (r = 1; r < oparg + 1; ++r) {
        f->regs[r] = stack->push_register(state->num_reg++);
      }
      break;
    }
//        case SETUP_EXCEPT:
//        case SETUP_FINALLY:
    case SETUP_LOOP:
      stack->push_frame(offset + CODESIZE(state->py_codestr[offset]) + oparg);
      bb->add_op(opcode, oparg);
      break;
    case RAISE_VARARGS:
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
      bb->add_op(opcode, oparg, r1, r2, r3);
      break;
    case POP_BLOCK:
      stack->pop_frame();
      bb->add_op(opcode, oparg);
      break;
      // Control flow instructions - recurse down each branch with a copy of the current stack.
    case BREAK_LOOP: {
      Frame *f = stack->pop_frame();
      bb->add_op(opcode, oparg);
      bb->exits.push_back(registerize(state, stack, f->target));
      if (bb->exits[0] == NULL) {
        return NULL;
      }
      return entry_point;
    }
    case CONTINUE_LOOP: {
      stack->pop_frame();
      bb->add_op(opcode, oparg);
      bb->exits.push_back(registerize(state, stack, oparg));
      if (bb->exits[0] == NULL) {
        return NULL;
      }
      break;
    }
    case FOR_ITER: {
      RegisterStack a, b;
      r1 = stack->pop_register();
      copy_stack(stack, &a);
      copy_stack(stack, &b);
      a.push_register(r1);
      r2 = a.push_register(state->num_reg++);

      bb->add_op(opcode, 0, r1, r2);

      // fall-through if iterator had an item, jump forward if iterator is empty.
      BasicBlock* left = registerize(state, &a, offset + CODESIZE(opcode));
      BasicBlock* right = registerize(state, &b, offset + CODESIZE(opcode) + oparg);
      bb->exits.push_back(left);
      bb->exits.push_back(right);
      if (left == NULL || right == NULL) {
        return NULL;
      }
      return entry_point;
    }
    case JUMP_IF_FALSE_OR_POP:
    case JUMP_IF_TRUE_OR_POP: {
      RegisterStack a, b;
      copy_stack(stack, &a);
      r1 = stack->pop_register();
      copy_stack(stack, &b);
      bb->add_op(opcode, oparg, r1);

      BasicBlock* right = registerize(state, &a, oparg);
      BasicBlock* left = registerize(state, &b, offset + CODESIZE(opcode));
      bb->exits.push_back(left);
      bb->exits.push_back(right);
      if (left == NULL || right == NULL) {
        return NULL;
      }
      return entry_point;
    }
    case POP_JUMP_IF_FALSE:
    case POP_JUMP_IF_TRUE: {
      r1 = stack->pop_register();
      RegisterStack a, b;
      copy_stack(stack, &a);
      copy_stack(stack, &b);
      bb->add_op(opcode, oparg, r1);
      BasicBlock* left = registerize(state, &a, offset + CODESIZE(opcode));
      BasicBlock* right = registerize(state, &b, oparg);
      bb->exits.push_back(left);
      bb->exits.push_back(right);
      if (left == NULL || right == NULL) {
        return NULL;
      }
      return entry_point;
    }
    case JUMP_FORWARD: {
      int dst = oparg + offset + CODESIZE(opcode);
      bb->add_op(JUMP_ABSOLUTE, dst);
      assert(dst <= state->py_codelen);
      BasicBlock* exit = registerize(state, stack, dst);
      bb->exits.push_back(exit);
      if (exit == NULL) {
        return NULL;
      }
      return entry_point;
    }
    case JUMP_ABSOLUTE: {
      bb->add_op(JUMP_ABSOLUTE, oparg);
      BasicBlock* exit = registerize(state, stack, oparg);
      bb->exits.push_back(exit);
      if (exit == NULL) {
        return NULL;
      }
      return entry_point;
    }
    case RETURN_VALUE:
      r1 = stack->pop_register();
      bb->add_op(opcode, oparg, r1);
      return entry_point;
    case END_FINALLY:
    case YIELD_VALUE:
    default:
//            fprintf(stderr, "Unknown opcode %s, arg = %d\n", opcode_to_name(opcode), oparg);
      return NULL;
      break;
    }
  }
  return entry_point;
}

typedef void (*BBPass)(BasicBlock*);
typedef void (*OpPass)(BasicBlock*, CompilerOp*);
void apply_bb_pass(CompilerState* state, BBPass pass) {
  for (size_t i = 0; i < state->bbs.size(); ++i) {
    state->bbs[i]->visited = 0;
  }

  for (size_t i = 0; i < state->bbs.size(); ++i) {
    if (!state->bbs[i]->visited) {
      pass(state->bbs[i]);
      state->bbs[i]->visited = 1;
    }
  }
}

void apply_op_pass(CompilerState* state, OpPass pass) {
  size_t i, j;
  for (i = 0; i < state->bbs.size(); ++i) {
    BasicBlock* bb = state->bbs[i];
    if (bb->dead) {
      continue;
    }
    for (j = 0; j < bb->code.size(); ++j) {
      if (!bb->code[j]->dead) {
        pass(bb, bb->code[j]);
      }
    }
  }

}

void bb_nop_pass(BasicBlock* bb) {
}

void bb_mark_entries_pass(BasicBlock* bb) {
  for (size_t i = 0; i < bb->exits.size(); ++i) {
    BasicBlock* next = bb->exits[i];
    next->entries.push_back(bb);
  }
}

void remove_dead_ops(BasicBlock* bb) {
  size_t j = 0;
  for (size_t i = 0; i < bb->code.size(); ++i) {
    CompilerOp * op = bb->code[i];
    if (op->dead) {
      continue;
    }
    bb->code[j++] = bb->code[i];
  }

  bb->code.resize(j);
}

void remove_dead_code(CompilerState* state) {
  size_t i = 0;
  size_t pos = 0;
  for (i = 0; i < state->bbs.size(); ++i) {
    BasicBlock* bb = state->bbs[i];
    if (bb->dead) {
      continue;
    }
    remove_dead_ops(bb);
    state->bbs[pos++] = bb;
  }
  state->bbs.resize(pos);
}

void bb_fuse_pass(BasicBlock* bb) {
  if (bb->visited || bb->dead || bb->exits.size() != 1) {
    fprintf(stderr, "Leaving %d alone.\n", bb->idx);
    return;
  }

  BasicBlock* next = bb->exits[0];
  while (1) {
    if (next->entries.size() > 1 || next->visited) {
      break;
    }

//        fprintf(stderr, "Merging %d into %d\n", next->idx, bb->idx);
    bb->code.insert(bb->code.end(), next->code.begin(), next->code.end());

    next->dead = next->visited = 1;
    bb->exits = next->exits;

    if (bb->exits.size() != 1) {
      break;
    }
    next = bb->exits[0];
  }
}

// For each basic block, find matching increfs and decrefs, and cancel them out.
void bb_combine_refs(BasicBlock* bb) {
  for (size_t i = 0; i < bb->code.size(); ++i) {
    CompilerOp * decref = bb->code[i];
    if (decref->dead || decref->code != DECREF) {
      continue;
    }
    for (size_t j = 0; j < i; ++j) {
      CompilerOp* incref = bb->code[j];
      if (!incref->dead && incref->code == INCREF && incref->regs[0] == decref->regs[0]) {
        decref->dead = 1;
        incref->dead = 1;
      }
    }
  }
}

void print_compiler_op(BasicBlock *bb, CompilerOp* op) {
  fprintf(stderr, "%d :: %s (ARG: %d, ", bb->idx, opcode_to_name(op->code), op->arg);
  for (size_t i = 0; i < op->regs.size(); ++i) {
    fprintf(stderr, "%d, ", op->regs[i]);
  }
  fprintf(stderr, ")");
  if (op->dead) {
    fprintf(stderr, " DEAD ");
  }

  if (op == bb->code[bb->code.size() - 1]) {
    fprintf(stderr, "-> [");
    for (size_t i = 0; i < bb->exits.size(); ++i) {
      fprintf(stderr, "%d,", bb->exits[i]->idx);
    }
    fprintf(stderr, "]");
  }
  fprintf(stderr, "\n");
}

typedef void (*OptPass)(CompilerState*);

void opt_fuse_bb(CompilerState* state) {
  apply_bb_pass(state, &bb_mark_entries_pass);
  apply_bb_pass(state, &bb_fuse_pass);
}

void opt_combine_refs(CompilerState* state) {
  apply_bb_pass(state, &bb_combine_refs);
}

void _apply_opt_pass(CompilerState* state, OptPass pass, const char* name) {
  //    fprintf(stderr, "Before %s:\n", name);
  //    apply_op_pass(state, &print_compiler_op);

  pass(state);

  remove_dead_code(state);

//    fprintf(stderr, "After %s:\n", name);
//    apply_op_pass(state, &print_compiler_op);

}

#define apply_opt_pass(state, pass) _apply_opt_pass(state, pass, #pass)

struct RCompilerUtil {
  static int op_size(CompilerOp* op) {
    if (is_varargs_op(op->code)) {
      return sizeof(RMachineOp) + sizeof(Register) * max(0, (int) op->regs.size() - 2);
    }
    return sizeof(RMachineOp);
  }

  static void write_op(char* dst, CompilerOp* op) {
    RMachineOp* dst_op = (RMachineOp*)dst;
    dst_op->header.code = op->code;
    dst_op->header.arg = op->arg;

    if (is_varargs_op(op->code)) {
      dst_op->varargs.num_registers = op->regs.size();
      for (size_t i = 0; i < op->regs.size(); ++i) {
        dst_op->varargs.regs[i] = op->regs[i];

        // Guard against overflowing our register size.
        assert(dst_op->varargs.regs[i] == op->regs[i]);
      }

      assert(dst_op->varargs.num_registers == op->regs.size());
    } else if (is_branch_op(op->code)) {
      assert(op->regs.size() < 3);
      dst_op->branch.reg_1 = op->regs.size() > 0 ? op->regs[0] : -1;
      dst_op->branch.reg_2 = op->regs.size() > 1 ? op->regs[1] : -1;
      dst_op->branch.label = 0;
    } else {
      assert(op->regs.size() <= 3);
      dst_op->reg.reg_1 = op->regs.size() > 0 ? op->regs[0] : -1;
      dst_op->reg.reg_2 = op->regs.size() > 1 ? op->regs[1] : -1;
      dst_op->reg.reg_3 = op->regs.size() > 2 ? op->regs[2] : -1;
    }
  }
};

void bb_to_code(CompilerState* state, std::string *out) {
  RegisterPrelude p;
  memcpy(&p.magic, REG_MAGIC, 4);
  p.mapped_registers = 0;
  p.mapped_labels = 0;
  p.num_registers = state->num_reg;
  out->append((char*) &p, sizeof(RegisterPrelude));

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
      RCompilerUtil::write_op(&(*out)[0] + offset, c);
      fprintf(stderr, "Wrote op at offset %d, size: %d\n", offset, RCompilerUtil::op_size(c));
      RMachineOp* rop = (RMachineOp*)(&(*out)[0] + offset);
      assert(RCompilerUtil::op_size(c) == RMachineOp::size(*rop));
    }
  }

  // now patchup labels in the emitted code to point to the correct
  // locations.
  Py_ssize_t pos = sizeof(RegisterPrelude);
  for (size_t i = 0; i < state->bbs.size(); ++i) {
    BasicBlock* bb = state->bbs[i];
    RMachineOp* op = NULL;

    // Skip to the end of the basic block.
    for (size_t j = 0; j < bb->code.size(); ++j) {
      op = (RMachineOp*) (out->data() + pos);
      fprintf(stderr, "Checking op %d at offset %d (original: %d)\n", op->code(), pos, bb->code[j]->code);

      assert(op->code() == bb->code[j]->code);
      assert(op->arg() == bb->code[j]->arg);
      pos += RMachineOp::size(*op);
    }

//    assert(is_branch_op(op->code()) || op->code() == RETURN_VALUE);

    if (is_branch_op(op->code()) && op->code() != RETURN_VALUE) {
      assert(bb->exits[0] != NULL);
      op->branch.label = bb->exits[0]->reg_offset;
      assert(bb->exits[0]->reg_offset > 0);
    }
  }
}

/*
 * Transform stack machine bytecode into registerized form.
 */
PyObject * RegisterCompileCode(PyCodeObject * code) {
  CompilerState state;
  RegisterStack stack;

  int codelen = PyString_GET_SIZE(code->co_code);

  state.num_consts = PyTuple_Size(code->co_consts);
  state.num_locals = code->co_nlocals;
  // Offset by the number of constants and locals.
  state.num_reg = state.num_consts + state.num_locals;
  fprintf(stderr, "Consts: %d, locals: %d, first register: %d\n", state.num_consts, state.num_locals, state.num_reg);

  state.py_codelen = codelen;
  state.py_codestr = (unsigned char*) PyString_AsString(code->co_code);

  BasicBlock* entry_point = registerize(&state, &stack, 0);
  if (entry_point == NULL) {
    fprintf(stderr, "Failed to registerize %s:%d (%s), using stack machine.\n", PyString_AsString(code->co_filename),
        code->co_firstlineno, PyString_AsString(code->co_name));
    return NULL;
  }

  apply_opt_pass(&state, &opt_fuse_bb);
  apply_opt_pass(&state, &opt_combine_refs);

  std::string regcode;
  bb_to_code(&state, &regcode);
  PyObject* regobj = PyString_FromStringAndSize((char*) regcode.data(), regcode.size());
  return regobj;
}
