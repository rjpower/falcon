#include "Python.h"
#include "opcode.h"
#include "reval.h"
#include "marshal.h"

#include <string.h>
#include <stdint.h>

#define CODE_FOR_OP(op_ptr) ((const RegisterOp*)op_ptr)->code
#define LABEL_FOR_OP(op_ptr) labels[CODE_FOR_OP(op_ptr)]

#define PRINT_OP(op) if (debug_print_ops) {print_op(op); }

#define JUMP_TO(op_ptr)\
    assert(CODE_FOR_OP(op_ptr) <= MAX_CODE);\
    goto *LABEL_FOR_OP(op_ptr)
//    goto *((RegisterOp*)(op_ptr))->handler

#define START_OP_FIXED(opname, num_registers)\
    op_##opname: {\
    const RegisterOp* const op = (RegisterOp*) (code + offset);\
    PRINT_OP(op)\
    const int jumpby = (sizeof(RegisterOp) + num_registers * sizeof(Register));\
    assert(jumpby == regop_size(op));\
    offset += jumpby;

#define START_OP_BRANCH(opname)\
    op_##opname: {\
    const RegisterOp* const op = (RegisterOp*) (code + offset);\
    PRINT_OP(op)\
    offset = op->branches[0];

#define END_OP JUMP_TO((code + offset)); }

#define BAD_OP(opname)\
        op_##opname: { fprintf(stderr, "Not implemented: %s\n", #opname); exit(1); }

#define FALLTHROUGH_OP(opname) op_##opname: 

#define BINARY_OP(opname, opfn)\
    op_##opname: {\
    RegisterOp* op = (RegisterOp*) (code + offset);\
    PRINT_OP(op)\
    offset += sizeof(RegisterOp) + 3 * sizeof(Register);\
    PyObject *r1 = registers[op->regs[0]];\
    PyObject *r2 = registers[op->regs[1]];\
    PyObject *r3 = opfn(r1, r2);\
    registers[op->regs[2]] = r3;\
    END_OP


PyObject * PyRegEval_EvalFrame(PyFrameObject *f, int throwflag) {
    const int debug_print_ops = getenv("PY_REG_PRINTOPS") != NULL;

    char* code;
    Py_ssize_t codelen;
    PyObject *result = NULL;

    PyString_AsStringAndSize(f->f_code->co_regcode, &code, &codelen);

    RegisterPrelude *prelude = (RegisterPrelude*) code;
    assert(memcmp(&prelude->magic, REG_MAGIC, 4) == 0);

    register int offset = sizeof(RegisterPrelude);

    // Remap branches to our actual labels.
//    if (!prelude->mapped_labels) {
//        prelude->mapped_labels = 1;
//        for (; offset < codelen; ) {
//            RegisterOp* op = (RegisterOp*)(code + offset);
//            op->handler = LABEL_FOR_OP(op);
//            offset += regop_size(op);
//        }
//    }

    PyObject* registers[prelude->num_registers];
    PyObject* const consts = f->f_code->co_consts;
    PyObject* const globals = f->f_globals;
    PyObject* const names = f->f_code->co_names;
    PyObject* const locals = f->f_locals;
    PyObject** fastlocals = f->f_localsplus;
    const int num_consts = PyTuple_Size(consts);
    const int num_locals = f->f_code->co_nlocals;

    // setup const and local register aliases.
    {
        int i;
        for (i = 0; i < num_consts; ++i) {
            registers[i] = PyTuple_GET_ITEM(consts, i);
        }

        for (i = 0; i < num_locals; ++i) {
            registers[num_consts + i] = fastlocals[i];
        }
    }


    fprintf(stderr, "New frame: %s:%d (%d %d %d)\n",
            PyString_AsString(f->f_code->co_filename), f->f_code->co_firstlineno,
            num_consts, num_locals, prelude->num_registers);

    static void *labels[] = {
        &&op_STOP_CODE,
        &&op_POP_TOP,
        &&op_ROT_TWO,
        &&op_ROT_THREE,
        &&op_DUP_TOP,
        &&op_ROT_FOUR,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_NOP,
        &&op_UNARY_POSITIVE,
        &&op_UNARY_NEGATIVE,
        &&op_UNARY_NOT,
        &&op_UNARY_CONVERT,
        &&op_BAD_OPCODE,
        &&op_UNARY_INVERT,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
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
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_STORE_SLICE,
        &&op_STORE_SLICE,
        &&op_STORE_SLICE,
        &&op_STORE_SLICE,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
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
        &&op_BAD_OPCODE,
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
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_CONTINUE_LOOP,
        &&op_SETUP_LOOP,
        &&op_SETUP_EXCEPT,
        &&op_SETUP_FINALLY,
        &&op_BAD_OPCODE,
        &&op_LOAD_FAST,
        &&op_STORE_FAST,
        &&op_DELETE_FAST,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_RAISE_VARARGS,
        &&op_CALL_FUNCTION,
        &&op_MAKE_FUNCTION,
        &&op_BUILD_SLICE,
        &&op_MAKE_CLOSURE,
        &&op_LOAD_CLOSURE,
        &&op_LOAD_DEREF,
        &&op_STORE_DEREF,
        &&op_BAD_OPCODE,
        &&op_BAD_OPCODE,
        &&op_CALL_FUNCTION_VAR,
        &&op_CALL_FUNCTION_KW,
        &&op_CALL_FUNCTION_VAR_KW,
        &&op_SETUP_WITH,
        &&op_BAD_OPCODE,
        &&op_EXTENDED_ARG,
        &&op_SET_ADD,
        &&op_MAP_ADD,
        &&op_INCREF,
        &&op_DECREF
    };

    offset = sizeof(RegisterPrelude);
    RegisterOp* first_op = (RegisterOp*) (code + offset);
//    print_op(first_op);

    // Let's start going!
    JUMP_TO(first_op);

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

    START_OP_FIXED(INCREF, 1)
        Py_INCREF(registers[op->regs[0]]);
    END_OP
    START_OP_FIXED(DECREF, 1)
        Py_XDECREF(registers[op->regs[0]]);
    END_OP
    START_OP_FIXED(LOAD_LOCALS, 1)
        registers[op->regs[0]] = locals;
        Py_INCREF(locals);
    END_OP
    START_OP_FIXED(LOAD_GLOBAL, 1)
        PyObject* r1 = PyTuple_GET_ITEM(names, op->arg);
        PyObject* r2 = PyDict_GetItem(globals, r1);
        if (r2 == NULL) {
            r2 = PyDict_GetItem(f->f_builtins, r1);
        }
        if (r2 == NULL) {
            PyErr_SetString(PyExc_NameError, "Global name XXX not defined.");
            return NULL;
        }
        registers[op->regs[0]] = r2;
    END_OP
    START_OP_FIXED(LOAD_NAME, 1)
        PyObject* r1 = PyTuple_GET_ITEM(names, op->arg);
        PyObject* r2 = PyDict_GetItem(locals, r1);
        if (r2 == NULL) {
            r2 = PyDict_GetItem(globals, r1);
        }
        if (r2 == NULL) {
            r2 = PyDict_GetItem(f->f_builtins, r1);
        }
        registers[op->regs[0]] = r2;
    END_OP
    START_OP_FIXED(STORE_FAST, 2)
        PyObject* from = registers[op->regs[0]];
        assert(from != NULL);
        registers[op->regs[1]] = from;
    END_OP
    START_OP_FIXED(STORE_NAME, 1)
        PyObject* r1 = PyTuple_GET_ITEM(names, op->arg);
        PyObject* r2 = registers[op->regs[0]];
        PyObject_SetItem(locals, r1, r2);
    END_OP
    START_OP_FIXED(STORE_ATTR, 2)
        PyObject* t = PyTuple_GET_ITEM(names, op->arg);
        PyObject* key = registers[op->regs[0]];
        PyObject* value = registers[op->regs[1]];
        PyObject_SetAttr(t, key, value);
    END_OP
    START_OP_FIXED(STORE_SUBSCR, 3)
        PyObject* key = registers[op->regs[0]];
        PyObject* list = registers[op->regs[1]];
        PyObject* value = registers[op->regs[2]];
        PyObject_SetItem(list, key, value);
    END_OP
    START_OP_FIXED(BINARY_SUBSCR, 3)
        PyObject* key = registers[op->regs[0]];
        PyObject* list = registers[op->regs[1]];
        registers[op->regs[2]] = PyObject_GetItem(list, key);
    END_OP
    START_OP_FIXED(LOAD_ATTR, 2)
        PyObject* obj = registers[op->regs[0]];
        PyObject* name = PyTuple_GET_ITEM(names, op->arg);
        registers[op->regs[1]] = PyObject_GetAttr(obj, name);
    END_OP
    START_OP_BRANCH(BUILD_TUPLE)
        int i;
        PyObject* t = PyTuple_New(op->arg);
        for (i = 0; i < op->arg; ++i) {
            PyTuple_SET_ITEM(t, i, registers[op->regs[i]]);
        }
        registers[op->regs[op->arg]] = t;
    END_OP
    START_OP_BRANCH(BUILD_LIST)
        int i;
        PyObject* t = PyList_New(op->arg);
        for (i = 0; i < op->arg; ++i) {
            PyList_SET_ITEM(t, i, registers[op->regs[i]]);
        }
        registers[op->regs[op->arg]] = t;
    END_OP
    FALLTHROUGH_OP(CALL_FUNCTION)
    FALLTHROUGH_OP(CALL_FUNCTION_VAR)
    FALLTHROUGH_OP(CALL_FUNCTION_KW)
    START_OP_BRANCH(CALL_FUNCTION_VAR_KW)
        int na = op->arg & 0xff;
        int nk = (op->arg >> 8) & 0xff;
        int n = nk * 2 + na;
        int i;
        PyObject* fn = registers[op->regs[n]];

        assert(n + 2 == op->num_registers);

        PyObject* args = PyTuple_New(na);
        for (i = 0; i < na; ++i) {
            PyTuple_SET_ITEM(args, i, registers[op->regs[i]]);
        }

        PyObject* kwdict = NULL;
        if (nk > 0) {
            kwdict = PyDict_New();
            for (i = na; i < nk * 2; i += 2) {
                PyDict_SetItem(kwdict, registers[op->regs[i]],
                        registers[op->regs[i + 1]]);
            }
        }

        PyObject* res = PyObject_Call(fn, args, kwdict);
        assert(res != NULL);
        registers[op->regs[n + 1]] = res;
    END_OP
    START_OP_FIXED(GET_ITER, 2)
        PyObject* res = PyObject_GetIter(registers[op->regs[0]]);
        registers[op->regs[1]] = res;
    END_OP
    START_OP_FIXED(FOR_ITER, 2)
        PyObject* r1 = PyIter_Next(registers[op->regs[0]]);
        if (r1) {
            registers[op->regs[1]] = r1;
            offset = op->branches[0];
        } else {
            offset = op->branches[1];
        }
    END_OP
    FALLTHROUGH_OP(POP_JUMP_IF_FALSE)
    START_OP_FIXED(JUMP_IF_FALSE_OR_POP, 1)
        assert(op->branches[0] > 0 && op->branches[1] > 0);
        offset = !PyObject_IsTrue(registers[op->regs[0]]) == 0 ? op->branches[1] : op->branches[0];
    END_OP
    FALLTHROUGH_OP(POP_JUMP_IF_TRUE)
    START_OP_FIXED(JUMP_IF_TRUE_OR_POP, 1)
        assert(op->branches[0] > 0 && op->branches[1] > 0);
        offset = PyObject_IsTrue(registers[op->regs[0]]) > 0 ? op->branches[1] : op->branches[0];
    END_OP
    START_OP_FIXED(RETURN_VALUE, 1)
        result = registers[op->regs[0]];
        Py_INCREF(result);
        goto done;
    END_OP
    FALLTHROUGH_OP(JUMP_FORWARD)
    FALLTHROUGH_OP(JUMP_ABSOLUTE)
    FALLTHROUGH_OP( SETUP_LOOP)
    START_OP_BRANCH(POP_BLOCK)
    END_OP

    BAD_OP(BAD_OPCODE);
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


done:
Py_INCREF(result);
fprintf(stderr, "Leaving reval frame... %p\n", result);
return result;
}
