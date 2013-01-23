#include <Python.h>
#include <opcode.h>
#include <marshal.h>
#include <string.h>
#include <stdint.h>

#include "reval.h"

struct RunState {
  char *code;
  Py_ssize_t codelen;

  int offset;
  PyObject** registers;
  PyObject* consts;
  PyObject* globals;
  PyObject* names;
  PyObject* locals;
  PyObject* result;
  PyObject** fastlocals;
  int num_consts;
  int num_locals;
  PyFrameObject* frame;

  RunState(PyRegisterFrame* r) {
    PyString_AsStringAndSize(r->regcode, &code, &codelen);

    RegisterPrelude *prelude = (RegisterPrelude*) code;
    assert(memcmp(&prelude->magic, REG_MAGIC, 4) == 0);

    offset = sizeof(RegisterPrelude);

    frame = r->frame;
    registers = (PyObject**) alloca(sizeof(PyObject*) * prelude->num_registers);
    consts = frame->f_code->co_consts;
    globals = frame->f_globals;
    names = frame->f_code->co_names;
    locals = frame->f_locals;
    fastlocals = frame->f_localsplus;
    num_consts = PyTuple_Size(consts);
    num_locals = frame->f_code->co_nlocals;
    result = NULL;

    // setup const and local register aliases.
    {
      int i;
      for (i = 0; i < num_consts; ++i) {
        registers[i] = PyTuple_GET_ITEM(consts, i) ;
      }

      for (i = 0; i < num_locals; ++i) {
        registers[num_consts + i] = fastlocals[i];
      }
    }

    fprintf(stderr, "New frame: %s:%d (%d %d %d)\n", PyString_AsString(frame->f_code->co_filename),
        frame->f_code->co_firstlineno, num_consts, num_locals, prelude->num_registers);
  }

  f_inline RegisterOp* next() {
    return (RegisterOp*) (code + offset);
  }

};

typedef PyObject* (*PyNumberFunction)(PyObject*, PyObject*);
#define CODE_FOR_OP(op_ptr) ((const RegisterOp*)op_ptr)->code
#define LABEL_FOR_OP(op_ptr) labels[CODE_FOR_OP(op_ptr)]
#define PRINT_OP(op) if (debug_print_ops) {print_op(op); }

typedef enum {
  EVAL_CONTINUE, EVAL_RETURN, EVAL_ERROR
} EvalStatus;

template<class OpType, class SubType>
struct Op {
  f_inline EvalStatus eval(RunState* state) {
    OpType op = *((OpType*) (state->code + state->offset));
    state->offset += sizeof(OpType);
    return ((SubType*) this)->_eval(op, state);
  }

  static SubType instance;
};

template<class SubType>
struct Op<VarRegOp, SubType> {
  f_inline EvalStatus eval(RunState* state) {
    VarRegOp *op = (VarRegOp*) (state->code + state->offset);
    state->offset += regop_size(op);
    return ((SubType*) this)->_eval(op, state);
  }

  static SubType instance;
};

template<int OpCode, PyNumberFunction F>
struct BinOp: public Op<RegOp4, BinOp<OpCode, F> > {
  f_inline EvalStatus _eval(RegOp4 op, RunState* state) {
    PyObject* r1 = state->registers[op.reg_1];
    PyObject* r2 = state->registers[op.reg_2];
    PyObject* r3 = F(r1, r2);
    state->registers[op.reg_3] = r3;
    return EVAL_CONTINUE;
  }
};

struct IncRef: public Op<RegOp4, IncRef> {
  f_inline EvalStatus _eval(RegOp4 op, RunState* state) {
    Py_INCREF(state->registers[op.reg_1]);
    return EVAL_CONTINUE;
  }
};

struct DecRef: public Op<RegOp4, DecRef> {
  f_inline EvalStatus _eval(RegOp4 op, RunState* state) {
    Py_XDECREF(state->registers[op.reg_1]);
    return EVAL_CONTINUE;
  }
};

struct LoadLocals: public Op<RegOp4, DecRef> {
  f_inline EvalStatus _eval(RegOp4 op, RunState* state) {
    state->registers[op.reg_1] = state->locals;
    Py_INCREF(state->locals);
    return EVAL_CONTINUE;
  }
};

struct LoadGlobal: public Op<RegOp4, DecRef> {
  f_inline EvalStatus _eval(RegOp4 op, RunState* state) {
    PyObject* r1 = PyTuple_GET_ITEM(state->names, op.arg) ;
    PyObject* r2 = PyDict_GetItem(state->globals, r1);
    if (r2 == NULL) {
      r2 = PyDict_GetItem(state->frame->f_builtins, r1);
    }
    if (r2 == NULL) {
      PyErr_SetString(PyExc_NameError, "Global name XXX not defined.");
      return EVAL_ERROR;
    }
    state->registers[op.reg_1] = r2;
    return EVAL_CONTINUE;
  }
};

struct LoadName: public Op<RegOp4, LoadName> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    PyObject* r1 = PyTuple_GET_ITEM(state->names, op.arg) ;
    PyObject* r2 = PyDict_GetItem(state->locals, r1);
    if (r2 == NULL) {
      r2 = PyDict_GetItem(state->globals, r1);
    }
    if (r2 == NULL) {
      r2 = PyDict_GetItem(state->frame->f_builtins, r1);
    }
    state->registers[op.reg_1] = r2;
    return EVAL_CONTINUE;
  }
};

struct StoreFast: public Op<RegOp4, StoreFast> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    PyObject* from = state->registers[op.reg_1];
    assert(from != NULL);
    state->registers[op.reg_2] = from;
    return EVAL_CONTINUE;
  }
};

struct StoreName: public Op<RegOp4, StoreName> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    PyObject* r1 = PyTuple_GET_ITEM(state->names, op.arg) ;
    PyObject* r2 = state->registers[op.reg_1];
    PyObject_SetItem(state->locals, r1, r2);
    return EVAL_CONTINUE;
  }
};

struct StoreAttr: public Op<RegOp4, StoreAttr> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    PyObject* t = PyTuple_GET_ITEM(state->names, op.arg) ;
    PyObject* key = state->registers[op.reg_1];
    PyObject* value = state->registers[op.reg_2];
    PyObject_SetAttr(t, key, value);
    return EVAL_CONTINUE;
  }
};

struct StoreSubscr: public Op<RegOp4, StoreSubscr> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    PyObject* key = state->registers[op.reg_1];
    PyObject* list = state->registers[op.reg_2];
    PyObject* value = state->registers[op.reg_3];
    PyObject_SetItem(list, key, value);
    return EVAL_CONTINUE;
  }
};

struct BinarySubscr: public Op<RegOp4, BinarySubscr> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    PyObject* key = state->registers[op.reg_1];
    PyObject* list = state->registers[op.reg_2];
    state->registers[op.reg_3] = PyObject_GetItem(list, key);
    return EVAL_CONTINUE;
  }
};

struct LoadAttr: public Op<RegOp4, LoadAttr> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    PyObject* obj = state->registers[op.reg_1];
    PyObject* name = PyTuple_GET_ITEM(state->names, op.arg) ;
    state->registers[op.reg_2] = PyObject_GetAttr(obj, name);
    return EVAL_CONTINUE;
  }
};
struct CallFunction: public Op<VarRegOp, CallFunction> {
  f_inline EvalStatus _eval(VarRegOp *op, RunState *state) {
    int na = op->arg & 0xff;
    int nk = (op->arg >> 8) & 0xff;
    int n = nk * 2 + na;
    int i;
    PyObject* fn = state->registers[op->regs[n]];

    assert( n + 2 == op->num_state->registers);

    PyObject* args = PyTuple_New(na);
    for (i = 0; i < na; ++i) {
      PyTuple_SET_ITEM(args, i, state->registers[op->regs[i]]);
    }

    PyObject* kwdict = NULL;
    if (nk > 0) {
      kwdict = PyDict_New();
      for (i = na; i < nk * 2; i += 2) {
        PyDict_SetItem(kwdict, state->registers[op->regs[i]], state->registers[op->regs[i + 1]]);
      }
    }

    PyObject* res = PyObject_Call(fn, args, kwdict);
    if (res == NULL) {
      return EVAL_ERROR;
    }

    state->registers[op->regs[n + 1]] = res;
    return EVAL_CONTINUE;
  }
};

struct GetIter: public Op<RegOp4, GetIter> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    PyObject* res = PyObject_GetIter(state->registers[op.reg_1]);
    state->registers[op.reg_2] = res;
    return EVAL_CONTINUE;
  }
};

struct ForIter: public Op<RegOp4, ForIter> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    PyObject* r1 = PyIter_Next(state->registers[op.reg_1]);
    if (r1) {
      state->registers[op.reg_2] = r1;
    } else {
      state->offset = op.branch;
    }
    return EVAL_CONTINUE;
  }
};

struct JumpIfFalseOrPop: public Op<RegOp4, JumpIfFalseOrPop> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    if (!PyObject_IsTrue(state->registers[op.reg_1]) == 0) {
      state->offset = op.branch;
    }
    return EVAL_CONTINUE;
  }
};

struct JumpIfTrueOrPop: public Op<RegOp4, JumpIfTrueOrPop> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    if (PyObject_IsTrue(state->registers[op.reg_1]) == 0) {
      state->offset = op.branch;
    }
    return EVAL_CONTINUE;
  }
};

struct ReturnValue: public Op<RegOp4, ReturnValue> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    state->result = state->registers[op.reg_1];
    Py_INCREF(state->result);
    return EVAL_RETURN;
  }
};

struct PopBlock: public Op<RegOp4, PopBlock> {
  f_inline EvalStatus _eval(RegOp4 op, RunState *state) {
    return EVAL_CONTINUE;
  }
};

struct BuildTuple: public Op<VarRegOp, BuildTuple> {
  f_inline EvalStatus _eval(VarRegOp *op, RunState *state) {
    int i;
    PyObject* t = PyTuple_New(op->arg);
    for (i = 0; i < op->arg; ++i) {
      PyTuple_SET_ITEM(t, i, state->registers[op->regs[i]]);
    }
    state->registers[op->regs[op->arg]] = t;
    return EVAL_CONTINUE;
  }
};

struct BuildList: public Op<VarRegOp, BuildList> {
  f_inline EvalStatus _eval(VarRegOp *op, RunState *state) {
    int i;
    PyObject* t = PyList_New(op->arg);
    for (i = 0; i < op->arg; ++i) {
      PyList_SET_ITEM(t, i, state->registers[op->regs[i]]);
    }
    state->registers[op->regs[op->arg]] = t;
    return EVAL_CONTINUE;
  }
};

struct LabelRegistry {
  static void* labels[256];
  static int add_label(int opcode, void* address) {
    labels[opcode] = address;
    return opcode;
  }
};

#define CONCAT(...) __VA_ARGS__

#define REGISTER_OP(opname)\
    static int _force_register_ ## opname = LabelRegistry::add_label(opname, &&op_ ## opname);

#define _DEFINE_OP(opname, impl){\
      switch (impl::instance.eval(&state)) {\
case EVAL_CONTINUE: goto *labels[0]; break;\
case EVAL_ERROR: goto error; break;\
case EVAL_RETURN: goto done; break;\
      }\
}

#define DEFINE_OP(opname, impl)\
    op_##opname: _DEFINE_OP(opname, impl)

#define BINARY_OP(opname, opfn)\
    op_##opname: _DEFINE_OP(opname, BinOp<CONCAT(opname, opfn)>)

#define _BAD_OP(opname)\
        { fprintf(stderr, "Not implemented: %s\n", #opname); abort(); }

#define BAD_OP(opname)\
    op_##opname: _BAD_OP(opname)

#define FALLTHROUGH(opname) op_##opname:

PyObject * PyRegEval_EvalFrame(PyRegisterFrame *r, int throwflag) {
  const int debug_print_ops = getenv("PY_REG_PRINTOPS") != NULL;
static void *labels[] = {
  &&op_STOP_CODE,
  &&op_POP_TOP,
  &&op_ROT_TWO,
  &&op_ROT_THREE,
  &&op_DUP_TOP,
  &&op_ROT_FOUR,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_NOP,
  &&op_UNARY_POSITIVE,
  &&op_UNARY_NEGATIVE,
  &&op_UNARY_NOT,
  &&op_UNARY_CONVERT,
  &&op_BADCODE,
  &&op_UNARY_INVERT,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BINARY_POWER,
  &&op_BINARY_MULTIPLY,
  &&op_BINARY_DIVIDE,
  &&op_BINARY_MODULO,
  &&op_BINARY_ADD,
  &&op_BINARY_SUBTRACT,
  &&op_BINARY_SUBSCR,
  &&op_BINARY_FLOOR_DIVIDE,
  &&op_BINARY_TRUE_DIVIDE,
  &&op_INPLACE_FLOOR_DIVIDE,
  &&op_INPLACE_TRUE_DIVIDE,
  &&op_SLICE,
  &&op_SLICE,
  &&op_SLICE,
  &&op_SLICE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_STORE_SLICE,
  &&op_STORE_SLICE,
  &&op_STORE_SLICE,
  &&op_STORE_SLICE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_DELETE_SLICE,
  &&op_DELETE_SLICE,
  &&op_DELETE_SLICE,
  &&op_DELETE_SLICE,
  &&op_STORE_MAP,
  &&op_INPLACE_ADD,
  &&op_INPLACE_SUBTRACT,
  &&op_INPLACE_MULTIPLY,
  &&op_INPLACE_DIVIDE,
  &&op_INPLACE_MODULO,
  &&op_STORE_SUBSCR,
  &&op_DELETE_SUBSCR,
  &&op_BINARY_LSHIFT,
  &&op_BINARY_RSHIFT,
  &&op_BINARY_AND,
  &&op_BINARY_XOR,
  &&op_BINARY_OR,
  &&op_INPLACE_POWER,
  &&op_GET_ITER,
  &&op_BADCODE,
  &&op_PRINT_EXPR,
  &&op_PRINT_ITEM,
  &&op_PRINT_NEWLINE,
  &&op_PRINT_ITEM_TO,
  &&op_PRINT_NEWLINE_TO,
  &&op_INPLACE_LSHIFT,
  &&op_INPLACE_RSHIFT,
  &&op_INPLACE_AND,
  &&op_INPLACE_XOR,
  &&op_INPLACE_OR,
  &&op_BREAK_LOOP,
  &&op_WITH_CLEANUP,
  &&op_LOAD_LOCALS,
  &&op_RETURN_VALUE,
  &&op_IMPORT_STAR,
  &&op_EXEC_STMT,
  &&op_YIELD_VALUE,
  &&op_POP_BLOCK,
  &&op_END_FINALLY,
  &&op_BUILD_CLASS,
  &&op_STORE_NAME,
  &&op_DELETE_NAME,
  &&op_UNPACK_SEQUENCE,
  &&op_FOR_ITER,
  &&op_LIST_APPEND,
  &&op_STORE_ATTR,
  &&op_DELETE_ATTR,
  &&op_STORE_GLOBAL,
  &&op_DELETE_GLOBAL,
  &&op_DUP_TOPX,
  &&op_LOAD_CONST,
  &&op_LOAD_NAME,
  &&op_BUILD_TUPLE,
  &&op_BUILD_LIST,
  &&op_BUILD_SET,
  &&op_BUILD_MAP,
  &&op_LOAD_ATTR,
  &&op_COMPARE_OP,
  &&op_IMPORT_NAME,
  &&op_IMPORT_FROM,
  &&op_JUMP_FORWARD,
  &&op_JUMP_IF_FALSE_OR_POP,
  &&op_JUMP_IF_TRUE_OR_POP,
  &&op_JUMP_ABSOLUTE,
  &&op_POP_JUMP_IF_FALSE,
  &&op_POP_JUMP_IF_TRUE,
  &&op_LOAD_GLOBAL,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_CONTINUE_LOOP,
  &&op_SETUP_LOOP,
  &&op_SETUP_EXCEPT,
  &&op_SETUP_FINALLY,
  &&op_BADCODE,
  &&op_LOAD_FAST,
  &&op_STORE_FAST,
  &&op_DELETE_FAST,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_RAISE_VARARGS,
  &&op_CALL_FUNCTION,
  &&op_MAKE_FUNCTION,
  &&op_BUILD_SLICE,
  &&op_MAKE_CLOSURE,
  &&op_LOAD_CLOSURE,
  &&op_LOAD_DEREF,
  &&op_STORE_DEREF,
  &&op_BADCODE,
  &&op_BADCODE,
  &&op_CALL_FUNCTION_VAR,
  &&op_CALL_FUNCTION_KW,
  &&op_CALL_FUNCTION_VAR_KW,
  &&op_SETUP_WITH,
  &&op_BADCODE,
  &&op_EXTENDED_ARG,
  &&op_SET_ADD,
  &&op_MAP_ADD,
  &&op_INCREF,
  &&op_DECREF
}
;

RunState state(r);
//goto *labels[state.next()->code];

BINARY_OP(BINARY_MULTIPLY, PyNumber_Multiply);
BINARY_OP(BINARY_DIVIDE, PyNumber_Divide);
BINARY_OP(BINARY_ADD, PyNumber_Add);
BINARY_OP(BINARY_SUBTRACT, PyNumber_Subtract);
BINARY_OP(BINARY_MODULO, PyNumber_Remainder);

BINARY_OP(INPLACE_MULTIPLY, PyNumber_InPlaceMultiply);
BINARY_OP(INPLACE_DIVIDE, PyNumber_InPlaceDivide);
BINARY_OP(INPLACE_ADD, PyNumber_InPlaceAdd);
BINARY_OP(INPLACE_SUBTRACT, PyNumber_InPlaceSubtract);
BINARY_OP(INPLACE_MODULO, PyNumber_InPlaceRemainder);

BAD_OP(LOAD_FAST)
BAD_OP(LOAD_CONST)

DEFINE_OP(INCREF, IncRef);
DEFINE_OP(DECREF, DecRef);
DEFINE_OP(LOAD_LOCALS, LoadLocals);
DEFINE_OP(LOAD_GLOBAL, LoadGlobal);
DEFINE_OP(LOAD_NAME, LoadName);
DEFINE_OP(LOAD_ATTR, LoadAttr);

DEFINE_OP(STORE_NAME, StoreName);
DEFINE_OP(STORE_ATTR, StoreAttr);
DEFINE_OP(STORE_SUBSCR, StoreSubscr);
DEFINE_OP(STORE_FAST, StoreFast);

DEFINE_OP(BINARY_SUBSCR, BinarySubscr);

DEFINE_OP(GET_ITER, GetIter);
DEFINE_OP(FOR_ITER, ForIter);
DEFINE_OP(RETURN_VALUE, ReturnValue);

DEFINE_OP(BUILD_TUPLE, BuildTuple);
DEFINE_OP(BUILD_LIST, BuildList);

FALLTHROUGH(CALL_FUNCTION);
FALLTHROUGH(CALL_FUNCTION_VAR);
FALLTHROUGH(CALL_FUNCTION_KW);
DEFINE_OP(CALL_FUNCTION_VAR_KW, CallFunction);

FALLTHROUGH(POP_JUMP_IF_FALSE);
DEFINE_OP(JUMP_IF_FALSE_OR_POP, JumpIfFalseOrPop);

FALLTHROUGH(POP_JUMP_IF_TRUE);
DEFINE_OP(JUMP_IF_TRUE_OR_POP, JumpIfTrueOrPop);

FALLTHROUGH(JUMP_FORWARD);
FALLTHROUGH(JUMP_ABSOLUTE)FALLTHROUGH( SETUP_LOOP)DEFINE_OP(POP_BLOCK, PopBlock);

op_BADCODE:
  { fprintf(stderr, "Bad opcode\n."); abort(); }

BAD_OP(MAP_ADD);
BAD_OP(SET_ADD);
BAD_OP(EXTENDED_ARG);
BAD_OP(SETUP_WITH);
BAD_OP(STORE_DEREF);
BAD_OP(LOAD_DEREF);
BAD_OP(LOAD_CLOSURE);
BAD_OP(MAKE_CLOSURE);
BAD_OP(BUILD_SLICE);
BAD_OP(MAKE_FUNCTION);
BAD_OP(RAISE_VARARGS);
BAD_OP(DELETE_FAST);
BAD_OP(SETUP_FINALLY);
BAD_OP(SETUP_EXCEPT);
BAD_OP(CONTINUE_LOOP);
BAD_OP(IMPORT_FROM);
BAD_OP(IMPORT_NAME);
BAD_OP(COMPARE_OP);
BAD_OP(BUILD_MAP);
BAD_OP(BUILD_SET);
BAD_OP(DUP_TOPX);
BAD_OP(DELETE_GLOBAL);
BAD_OP(STORE_GLOBAL);
BAD_OP(DELETE_ATTR);
BAD_OP(LIST_APPEND);
BAD_OP(UNPACK_SEQUENCE);
BAD_OP(DELETE_NAME);
BAD_OP(BUILD_CLASS);
BAD_OP(END_FINALLY);
BAD_OP(YIELD_VALUE);
BAD_OP(EXEC_STMT);
BAD_OP(IMPORT_STAR);
BAD_OP(WITH_CLEANUP);
BAD_OP(BREAK_LOOP);
BAD_OP(INPLACE_OR);
BAD_OP(INPLACE_XOR);
BAD_OP(INPLACE_AND);
BAD_OP(INPLACE_RSHIFT);
BAD_OP(INPLACE_LSHIFT);
BAD_OP(PRINT_NEWLINE_TO);
BAD_OP(PRINT_ITEM_TO);
BAD_OP(PRINT_NEWLINE);
BAD_OP(PRINT_ITEM);
BAD_OP(PRINT_EXPR);
BAD_OP(INPLACE_POWER);
BAD_OP(BINARY_OR);
BAD_OP(BINARY_XOR);
BAD_OP(BINARY_AND);
BAD_OP(BINARY_RSHIFT);
BAD_OP(BINARY_LSHIFT);
BAD_OP(DELETE_SUBSCR);
BAD_OP(STORE_MAP);
BAD_OP(DELETE_SLICE);
BAD_OP(STORE_SLICE);
BAD_OP(SLICE);
BAD_OP(INPLACE_TRUE_DIVIDE);
BAD_OP(INPLACE_FLOOR_DIVIDE);
BAD_OP(BINARY_TRUE_DIVIDE);
BAD_OP(BINARY_FLOOR_DIVIDE);
BAD_OP(BINARY_POWER);
BAD_OP(UNARY_INVERT);
BAD_OP(UNARY_CONVERT);
BAD_OP(UNARY_NOT);
BAD_OP(UNARY_NEGATIVE);
BAD_OP(UNARY_POSITIVE);
BAD_OP(NOP);
BAD_OP(ROT_FOUR);
BAD_OP(DUP_TOP);
BAD_OP(ROT_THREE);
BAD_OP(ROT_TWO);
BAD_OP(POP_TOP);
BAD_OP(STOP_CODE);

error: return NULL;

done: return state.result;
}
