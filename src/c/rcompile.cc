#include "Python.h"

#include "Python-ast.h"
#include "node.h"
#include "pyarena.h"
#include "ast.h"
#include "code.h"
#include "compile.h"
#include "symtable.h"
#include "opcode.h"
#include "marshal.h"
#include "reval.h"

#include <string.h>
#include <stdint.h>

#include <vector>
#include <string>

#define GETARG(arr, i) ((int)((arr[i+2]<<8) + arr[i+1]))
#define CODESIZE(op)  (HAS_ARG(op) ? 3 : 1)

int is_branch_op(RegisterOp* op) {
    int opcode = op->code;
    return (opcode == POP_JUMP_IF_FALSE || opcode == POP_JUMP_IF_TRUE
            || opcode == JUMP_IF_FALSE_OR_POP || opcode == JUMP_IF_TRUE_OR_POP
            || opcode == JUMP_ABSOLUTE || opcode == JUMP_FORWARD
            || opcode == FOR_ITER);
}

// While compiling, we use an expanded form to represent opcodes.  This
// is translated to a compact instruction stream as the last compilation
// step.
typedef struct {
    int code;
    int arg;

    // this instruction has been marked dead by an optimization pass,
    // and should be ignored.
    int dead;

    JumpLoc branches[2];
    std::vector<Register> regs;
} CompilerOp;

static void compilerop_to_regop(CompilerOp* c, RegisterOp* r) {
    r->code = c->code;
    r->arg = c->arg;
    r->branches[0] = c->branches[0];
    r->branches[1] = c->branches[1];
    r->num_registers = c->regs.size();
    for (size_t i = 0; i < c->regs.size(); ++i) {
        r->regs[i] = c->regs[i];
    }
}

struct BasicBlock;

typedef struct BasicBlock {
    int py_offset;
    int reg_offset;
    int idx;

    std::vector<BasicBlock*> exits;
    std::vector<BasicBlock*> entries;
    std::vector<CompilerOp*> code;

    // Have we been visited by the current pass already?
    int visited;
    int dead;
} BasicBlock;

static void init_bb(BasicBlock* bb, int offset, int idx) {
    bb->py_offset = offset;
    bb->visited = 0;
    bb->dead = 0;
    bb->idx = idx;
}

typedef struct {
    int target;
    int stack_pos;
} RegisterFrame;

typedef struct {
    int regs[REG_MAX_STACK];
    int stack_pos;

    RegisterFrame frames[REG_MAX_FRAMES];
    int num_frames;
} RegisterStack;

typedef struct {
    std::vector<BasicBlock*> bbs;

    unsigned char* py_codestr;
    Py_ssize_t py_codelen;

    int num_reg;
    int num_consts;
    int num_locals;
} CompilerState;

static void init_state(CompilerState* state) {
}

static BasicBlock* alloc_bb(CompilerState* state) {
    BasicBlock* bb = new BasicBlock;
    state->bbs.push_back(bb);
    return bb;
}

static void push_frame(RegisterStack* c, int target) {
    assert(c->num_frames < REG_MAX_FRAMES);
    RegisterFrame* f = &c->frames[c->num_frames++];
    f->target = target;
    f->stack_pos = c->stack_pos;
}

static RegisterFrame* pop_frame(RegisterStack* c) {
    assert(c->num_frames > 0);
    RegisterFrame* f = &c->frames[--c->num_frames];
    c->stack_pos = f->stack_pos;
    return f;
}

static int push_register(RegisterStack* st, int reg) {
    // fprintf(stderr, "Pushing register %d, pos %d\n", reg, st->stack_pos + 1);
    assert(st->stack_pos < REG_MAX_STACK);
    st->regs[++st->stack_pos] = reg;
    return reg;
}

static int pop_register(RegisterStack* st) {
    assert(st->stack_pos >= 0);
    int reg = st->regs[st->stack_pos--];
    assert(reg >= -1);
    // fprintf(stderr, "Popped register %d, pos: %d\n", reg, st->stack_pos + 1);
    return reg;
}

static int peek_register(RegisterStack* st, int reg) {
    return st->regs[st->stack_pos - reg];
}

void print_stack(RegisterStack* st) {
    int i;
    fprintf(stderr, "[");
    for (i = 0; i <= st->stack_pos; ++i) {
        fprintf(stderr, "%d, ", st->regs[i]);
    }
    fprintf(stderr, "]\n");
}

static void copy_stack(RegisterStack *from, RegisterStack* to) {
    memcpy(to->regs, from->regs, sizeof(from->regs));
    to->stack_pos = from->stack_pos;
    memcpy(to->frames, from->frames, sizeof(from->frames));
    to->num_frames = from->num_frames;
}

static CompilerOp* _add_op(BasicBlock* bb, int opcode, int arg, int num_regs) {
    CompilerOp* op = new CompilerOp;
    op->code = opcode;
    op->regs.resize(num_regs);
    op->branches[0] = -1;
    op->branches[1] = -1;
    op->dead = 0;
    op->arg = arg;
    bb->code.push_back(op);

    return op;
}

static void add_op(BasicBlock* bb, int opcode, int arg, Register reg1,
        Register reg2, Register reg3, Register reg4) {
    int num_regs = 0;

    if (reg1 != -1) {
        num_regs = 1;
    }
    if (reg2 != -1) {
        num_regs = 2;
    }
    if (reg3 != -1) {
        num_regs = 3;
    }
    if (reg4 != -1) {
        num_regs = 4;
    }

    CompilerOp* op = _add_op(bb, opcode, arg, num_regs);

    switch (num_regs) {
    case 4:
        op->regs[3] = reg4;
    case 3:
        op->regs[2] = reg3;
    case 2:
        op->regs[1] = reg2;
    case 1:
        op->regs[0] = reg1;
    case 0:
        break;
    default:
        assert(0 && "Invalid number of registers for op?");
        break;
    }
}

static CompilerOp* add_varargs_op(BasicBlock* bb, int opcode, int arg,
        int num_regs) {
    CompilerOp* op = _add_op(bb, opcode, arg, num_regs);
    return op;
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
static BasicBlock* registerize(CompilerState* state, RegisterStack *stack,
        int offset) {
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
            r1 = pop_register(stack);
            r2 = pop_register(stack);
            push_register(stack, r1);
            push_register(stack, r2);
            continue;
        case ROT_THREE:
            r1 = pop_register(stack);
            r2 = pop_register(stack);
            r3 = pop_register(stack);
            push_register(stack, r1);
            push_register(stack, r3);
            push_register(stack, r2);
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

        BasicBlock *bb = alloc_bb(state);
        if (!entry_point) {
            entry_point = bb;
        }

        if (last) {
            last->exits.push_back(bb);
        }

        last = bb;
        init_bb(bb, offset, state->bbs.size() - 1);
        switch (opcode) {
        // Stack pushing/popping
        case POP_TOP:
            r1 = pop_register(stack);
            add_op(bb, DECREF, 0, r1, -1, -1, -1);
            break;
        case DUP_TOP:
            r1 = pop_register(stack);
            push_register(stack, r1);
            push_register(stack, r1);
            add_op(bb, INCREF, 0, r1, -1, -1, -1);
            break;
        case DUP_TOPX:
            if (oparg == 2) {
                r1 = pop_register(stack);
                r2 = pop_register(stack);
                add_op(bb, INCREF, 0, r1, -1, -1, -1);
                add_op(bb, INCREF, 0, r2, -1, -1, -1);
                push_register(stack, r1);
                push_register(stack, r2);
                push_register(stack, r1);
                push_register(stack, r2);
            } else {
                r1 = pop_register(stack);
                r2 = pop_register(stack);
                r3 = pop_register(stack);
                add_op(bb, INCREF, 0, r1, -1, -1, -1);
                add_op(bb, INCREF, 0, r2, -1, -1, -1);
                add_op(bb, INCREF, 0, r3, -1, -1, -1);
                push_register(stack, r3);
                push_register(stack, r2);
                push_register(stack, r1);
                push_register(stack, r3);
                push_register(stack, r2);
                push_register(stack, r1);
            }
            break;
            // Load operations: push one register onto the stack.
        case LOAD_CONST:
            r1 = push_register(stack, oparg);
            add_op(bb, INCREF, 0, r1, -1, -1, -1);
            break;
        case LOAD_FAST:
            r1 = push_register(stack, state->num_consts + oparg);
            add_op(bb, INCREF, 0, r1, -1, -1, -1);
            break;
        case LOAD_CLOSURE:
        case LOAD_DEREF:
        case LOAD_GLOBAL:
        case LOAD_LOCALS:
        case LOAD_NAME:
            r1 = push_register(stack, state->num_reg++);
            add_op(bb, opcode, oparg, r1, -1, -1, -1);
            add_op(bb, INCREF, 0, r1, -1, -1, -1);
            break;
        case LOAD_ATTR:
            r1 = pop_register(stack);
            r2 = push_register(stack, state->num_reg++);
            add_op(bb, opcode, oparg, r1, r2, -1, -1);
            add_op(bb, DECREF, 0, r1, -1, -1, -1);
            break;
        case STORE_FAST:
            r1 = pop_register(stack);
            // Decrement the old value.
            add_op(bb, DECREF, 0, state->num_consts + oparg, -1, -1, -1);
            add_op(bb, opcode, 0, r1, state->num_consts + oparg, -1, -1);
            break;
            // Store operations remove one or more registers from the stack.
        case STORE_DEREF:
        case STORE_GLOBAL:
        case STORE_NAME:
            r1 = pop_register(stack);
            add_op(bb, opcode, oparg, r1, -1, -1, -1);
            add_op(bb, DECREF, 0, r1, -1, -1, -1);
            break;
        case STORE_ATTR:
            r1 = pop_register(stack);
            r2 = pop_register(stack);
            add_op(bb, opcode, oparg, r1, r2, -1, -1);
            add_op(bb, DECREF, 0, r1, -1, -1, -1);
            add_op(bb, DECREF, 0, r2, -1, -1, -1);
            break;
        case STORE_MAP:
            r1 = pop_register(stack);
            r2 = pop_register(stack);
            r3 = pop_register(stack);
            add_op(bb, opcode, oparg, r1, r2, r3, -1);
            push_register(stack, r3);
            add_op(bb, DECREF, 0, r1, -1, -1, -1);
            add_op(bb, DECREF, 0, r2, -1, -1, -1);
            break;
        case STORE_SUBSCR:
            r1 = pop_register(stack);
            r2 = pop_register(stack);
            r3 = pop_register(stack);
            add_op(bb, opcode, oparg, r1, r2, r3, -1);
            add_op(bb, DECREF, 0, r1, -1, -1, -1);
            add_op(bb, DECREF, 0, r2, -1, -1, -1);
            add_op(bb, DECREF, 0, r3, -1, -1, -1);
            break;
        case GET_ITER:
            r1 = pop_register(stack);
            r2 = push_register(stack, state->num_reg++);
            add_op(bb, opcode, oparg, r1, r2, -1, -1);
            add_op(bb, DECREF, oparg, r1, -1, -1, -1);
            break;
        case SLICE + 0:
        case SLICE + 1:
        case SLICE + 2:
        case SLICE + 3:
            r1 = r2 = r3 = r4 = -1;
            if ((opcode - SLICE) & 2)
                r3 = pop_register(stack);
            if ((opcode - SLICE) & 1)
                r2 = pop_register(stack);
            r1 = pop_register(stack);
            r4 = push_register(stack, state->num_reg++);

            if (r2 == -1) {
                add_op(bb, opcode, oparg, r1, r4, -1, -1);
            } else {
                if (r3 == -1) {
                    add_op(bb, opcode, oparg, r1, r2, r4, -1);
                } else {
                    add_op(bb, opcode, oparg, r1, r2, r3, r4);
                }
            }
            break;
        case STORE_SLICE + 0:
        case STORE_SLICE + 1:
        case STORE_SLICE + 2:
        case STORE_SLICE + 3:
            r1 = r2 = r3 = r4 = -1;
            if ((opcode - STORE_SLICE) & 2)
                r4 = pop_register(stack);
            if ((opcode - STORE_SLICE) & 1)
                r3 = pop_register(stack);
            r2 = pop_register(stack);
            r1 = pop_register(stack);
            if (r3 == -1) {
                add_op(bb, opcode, oparg, r1, r2, -1, -1);
            } else {
                if (r4 == -1) {
                    add_op(bb, opcode, oparg, r1, r2, r3, -1);
                } else {
                    add_op(bb, opcode, oparg, r1, r2, r3, r4);
                }
            }
            break;
        case DELETE_SLICE + 0:
        case DELETE_SLICE + 1:
        case DELETE_SLICE + 2:
        case DELETE_SLICE + 3:
            r1 = r2 = r3 = r4 = -1;
            if ((opcode - DELETE_SLICE) & 2)
                r4 = pop_register(stack);
            if ((opcode - DELETE_SLICE) & 1)
                r3 = pop_register(stack);
            r2 = pop_register(stack);
            r1 = pop_register(stack);
            if (r3 == -1) {
                add_op(bb, opcode, oparg, r1, r2, -1, -1);
            } else {
                if (r4 == -1) {
                    add_op(bb, opcode, oparg, r1, r2, r3, -1);
                } else {
                    add_op(bb, opcode, oparg, r1, r2, r3, r4);
                }
            }
            break;
        case LIST_APPEND:
            r1 = pop_register(stack);
            r2 = peek_register(stack, oparg);
            add_op(bb, opcode, oparg, r1, r2, -1, -1);
            break;
        case UNARY_NOT:
            // Unary operations: pop 1, push 1.
            r1 = pop_register(stack);
            r2 = push_register(stack, state->num_reg++);
            add_op(bb, opcode, oparg, r1, r2, -1, -1);
            break;
        case UNARY_POSITIVE:
        case UNARY_NEGATIVE:
        case UNARY_CONVERT:
        case UNARY_INVERT:
            // Unary operations: pop 1, push 1.
            r1 = pop_register(stack);
            r2 = push_register(stack, state->num_reg++);
            add_op(bb, opcode, oparg, r1, r2, -1, -1);
            add_op(bb, DECREF, 0, r1, -1, -1, -1);
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
            r1 = pop_register(stack);
            r2 = pop_register(stack);
            r3 = push_register(stack, state->num_reg++);
            add_op(bb, opcode, oparg, r1, r2, r3, -1);
            add_op(bb, DECREF, 0, r1, -1, -1, -1);
            add_op(bb, DECREF, 0, r2, -1, -1, -1);
            break;
        case CALL_FUNCTION:
        case CALL_FUNCTION_VAR:
        case CALL_FUNCTION_KW:
        case CALL_FUNCTION_VAR_KW: {
            int na = oparg & 0xff;
            int nk = (oparg >> 8) & 0xff;
            int n = na + 2 * nk;
            CompilerOp* f = add_varargs_op(bb, opcode, oparg, n + 2);
            for (r = n - 1; r >= 0; --r) {
                f->regs[r] = pop_register(stack);
            }
            f->regs[n] = pop_register(stack);
            f->regs[n + 1] = push_register(stack, state->num_reg++);
            assert(f->arg == oparg);

//            for (r = n; r >= 0; --r) { add_op(bb, DECREF, 0, f->regs[r], -1, -1, -1); }
            break;
        }
        case BUILD_LIST:
        case BUILD_SET:
        case BUILD_TUPLE: {
            CompilerOp* f = add_varargs_op(bb, opcode, oparg, oparg + 1);
            for (r = 0; r < oparg; ++r) {
                f->regs[r] = pop_register(stack);
            }
            f->regs[oparg] = push_register(stack, state->num_reg++);
            break;
        }
        case UNPACK_SEQUENCE: {
            CompilerOp* f = add_varargs_op(bb, opcode, oparg, oparg + 1);
            f->regs[0] = pop_register(stack);
            for (r = 1; r < oparg + 1; ++r) {
                f->regs[r] = push_register(stack, state->num_reg++);
            }
            break;
        }
//        case SETUP_EXCEPT:
//        case SETUP_FINALLY:
        case SETUP_LOOP:
            push_frame(stack,
                    offset + CODESIZE(state->py_codestr[offset]) + oparg);
            add_op(bb, opcode, oparg, -1, -1, -1, -1);
            break;
        case RAISE_VARARGS:
            r1 = r2 = r3 = -1;
            if (oparg == 3) {
                r1 = pop_register(stack);
                r2 = pop_register(stack);
                r3 = pop_register(stack);
            } else if (oparg == 2) {
                r1 = pop_register(stack);
                r2 = pop_register(stack);
            } else if (oparg == 1) {
                r1 = pop_register(stack);
            }
            add_op(bb, opcode, oparg, r1, r2, r3, -1);
            break;
        case POP_BLOCK:
            pop_frame(stack);
            add_op(bb, opcode, oparg, -1, -1, -1, -1);
            break;
            // Control flow instructions - recurse down each branch with a copy of the current stack.
        case BREAK_LOOP: {
            RegisterFrame *f = pop_frame(stack);
            add_op(bb, opcode, oparg, -1, -1, -1, -1);
            bb->exits.push_back(registerize(state, stack, f->target));
            if (bb->exits[0] == NULL) {
                return NULL;
            }
            return entry_point;
        }
        case CONTINUE_LOOP: {
            pop_frame(stack);
            add_op(bb, opcode, oparg, -1, -1, -1, -1);
            bb->exits.push_back(registerize(state, stack, oparg));
            if (bb->exits[0] == NULL) {
                return NULL;
            }
            break;
        }
        case FOR_ITER: {
            RegisterStack a, b;
            r1 = pop_register(stack);
            copy_stack(stack, &a);
            copy_stack(stack, &b);
            push_register(&a, r1);
            r2 = push_register(&a, state->num_reg++);

            add_op(bb, opcode, 0, r1, r2, -1, -1);

            // fall-through if iterator had an item, jump forward if iterator is empty.
            BasicBlock* left = registerize(state, &a,
                    offset + CODESIZE(opcode));
            BasicBlock* right = registerize(state, &b,
                    offset + CODESIZE(opcode) + oparg);
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
            r1 = pop_register(stack);
            copy_stack(stack, &b);
            add_op(bb, opcode, oparg, r1, -1, -1, -1);

            BasicBlock* right = registerize(state, &a, oparg);
            BasicBlock* left = registerize(state, &b,
                    offset + CODESIZE(opcode));
            bb->exits.push_back(left);
            bb->exits.push_back(right);
            if (left == NULL || right == NULL) {
                return NULL;
            }
            return entry_point;
        }
        case POP_JUMP_IF_FALSE:
        case POP_JUMP_IF_TRUE: {
            r1 = pop_register(stack);
            RegisterStack a, b;
            copy_stack(stack, &a);
            copy_stack(stack, &b);
            add_op(bb, opcode, oparg, r1, -1, -1, -1);
            BasicBlock* left = registerize(state, &a,
                    offset + CODESIZE(opcode));
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
            add_op(bb, JUMP_ABSOLUTE, dst, -1, -1, -1, -1);
            assert(dst <= state->py_codelen);
            BasicBlock* exit = registerize(state, stack, dst);
            bb->exits.push_back(exit);
            if (exit == NULL) {
                return NULL;
            }
            return entry_point;
        }
        case JUMP_ABSOLUTE: {
            add_op(bb, JUMP_ABSOLUTE, oparg, -1, -1, -1, -1);
            BasicBlock* exit = registerize(state, stack, oparg);
            bb->exits.push_back(exit);
            if (exit == NULL) {
                return NULL;
            }
            return entry_point;
        }
        case RETURN_VALUE:
            r1 = pop_register(stack);
            add_op(bb, opcode, oparg, r1, -1, -1, -1);
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
static void apply_bb_pass(CompilerState* state, BBPass pass) {
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

static void apply_op_pass(CompilerState* state, OpPass pass) {
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

static void bb_nop_pass(BasicBlock* bb) {
}

static void bb_mark_entries_pass(BasicBlock* bb) {
    for (size_t i = 0; i < bb->exits.size(); ++i) {
        BasicBlock* next = bb->exits[i];
        next->entries.push_back(bb);
    }
}

static void remove_dead_ops(BasicBlock* bb) {
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

static void remove_dead_code(CompilerState* state) {
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

static void bb_fuse_pass(BasicBlock* bb) {
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
static void bb_combine_refs(BasicBlock* bb) {
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

static void print_compiler_op(BasicBlock *bb, CompilerOp* op) {
    fprintf(stderr, "%d :: %s (ARG: %d, ", bb->idx, opcode_to_name(op->code),
            op->arg);
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

static void opt_fuse_bb(CompilerState* state) {
    apply_bb_pass(state, &bb_mark_entries_pass);
    apply_bb_pass(state, &bb_fuse_pass);
}

static void opt_combine_refs(CompilerState* state) {
    apply_bb_pass(state, &bb_combine_refs);
}

static void _apply_opt_pass(CompilerState* state, OptPass pass,
        const char* name) {
    //    fprintf(stderr, "Before %s:\n", name);
    //    apply_op_pass(state, &print_compiler_op);

    pass(state);

    remove_dead_code(state);

//    fprintf(stderr, "After %s:\n", name);
//    apply_op_pass(state, &print_compiler_op);

}

#define apply_opt_pass(state, pass) _apply_opt_pass(state, pass, #pass)

static void bb_to_code(CompilerState* state, std::string *out) {
    RegisterPrelude p;
    memcpy(&p.magic, REG_MAGIC, 4);
    p.mapped_registers = 0;
    p.mapped_labels = 0;
    p.num_registers = state->num_reg;
    out->append((char*) &p, sizeof(RegisterPrelude));

//    fprintf(stderr, "Converting compiler ops to register ops.\n");
//    apply_op_pass(state, &print_compiler_op);

    // first, dump all of the operations to the output buffer and record
    // their positions.
    for (size_t i = 0; i < state->bbs.size(); ++i) {
        BasicBlock* bb = state->bbs[i];
        assert(!bb->dead);
        bb->reg_offset = out->size();
        for (size_t j = 0; j < bb->code.size(); ++j) {
            CompilerOp* c = bb->code[j];
            assert(!c->dead);

            int rop_size = sizeof(RegisterOp)
                    + sizeof(Register) * c->regs.size();
            size_t offset = out->size();
            out->resize(out->size() + rop_size);
            RegisterOp* r = (RegisterOp*) (out->data() + offset);
            compilerop_to_regop(c, r);
        }
    }

    // now patchup the emitted code.
    Py_ssize_t pos = sizeof(RegisterPrelude);
    for (size_t i = 0; i < state->bbs.size(); ++i) {
        BasicBlock* bb = state->bbs[i];
        RegisterOp *prev = NULL;
        RegisterOp *op = NULL;
        for (size_t j = 0; j < bb->code.size(); ++j) {
            if (prev) {
                prev->branches[0] = pos;
                prev->branches[1] = -1;
            }

            op = (RegisterOp*) (out->data() + pos);
            assert(op->code <= MAX_CODE);

            //            fprintf(stderr, "Reading %d.%d:", i, j);
            //            print_op(op);
            prev = op;
            pos += regop_size(op);
        }

        if (op->code != RETURN_VALUE) {
            assert(bb->exits[0] != NULL);
            op->branches[0] = bb->exits[0]->reg_offset;
            assert(bb->exits[0]->reg_offset > 0);
        }

        if (bb->exits.size() > 1) {
            assert(bb->exits.size() == 2);
            op->branches[1] = bb->exits[1]->reg_offset;
            assert(bb->exits[1]->reg_offset > 0);
        } else {
            op->branches[1] = -1;
        }
    }
}

/*
 * Transform stack machine bytecode into registerized form.
 */
PyObject * PyCode_Registerize(PyCodeObject * code) {
    if (getenv("PY_NO_REGISTER")) {
        return NULL;
    }

    CompilerState *state = new CompilerState;
    RegisterStack stack;

    int codelen = PyString_GET_SIZE(code->co_code);
    stack.stack_pos = -1;
    stack.num_frames = 0;

    init_state(state);
    state->num_consts = PyTuple_Size(code->co_consts);
    state->num_locals = code->co_nlocals;
    // Offset by the number of constants and locals.
    state->num_reg = state->num_consts + state->num_locals;
//    fprintf(stderr, "Consts: %d, locals: %d, first register: %d\n",
//            state->num_consts, state->num_locals, num_reg);

    state->py_codelen = codelen;
    state->py_codestr = (unsigned char*) PyString_AsString(code->co_code);

    BasicBlock* entry_point = registerize(state, &stack, 0);
    if (entry_point == NULL) {
//        fprintf(stderr,
//                "Failed to registerize %s:%d (%s), using stack machine.\n",
//                PyString_AsString(code->co_filename), code->co_firstlineno,
//                PyString_AsString(code->co_name));
        return NULL;
    }

    apply_opt_pass(state, &opt_fuse_bb);
    apply_opt_pass(state, &opt_combine_refs);

    std::string regcode;
    bb_to_code(state, &regcode);
    PyObject* regobj = PyString_FromStringAndSize((char*) regcode.data(),
            regcode.size());
    delete state;
    return regobj;
}
