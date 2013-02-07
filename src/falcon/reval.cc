#include <Python.h>
#include <opcode.h>
#include <marshal.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "reval.h"
#include "rcompile.h"

const char* obj_to_str(PyObject* o) {
  return PyString_AsString(PyObject_Str(o));
}

#ifdef FALCON_DEBUG
//#define EVAL_LOG(...) fprintf(stderr, __VA_ARGS__); fputs("\n", stderr);
#define EVAL_LOG(...)
#else
#define EVAL_LOG(...)
#endif

typedef PyObject* (*PyNumberFunction)(PyObject*, PyObject*);

struct EvalError {
  PyObject* exception;
  PyObject* value;

  EvalError() {
    assert(PyErr_Occurred());
    exception = NULL;
    value = NULL;
  }

  EvalError(PyObject* exc) :
      exception(exc), value(NULL) {
  }

  EvalError(PyObject* exc, const char* fmt, ...) :
      exception(exc) {
    va_list vargs;
    va_start(vargs, fmt);
    value = PyString_FromFormatV(fmt, vargs);
    va_end(vargs);
  }
};

struct GilHelper {
  PyGILState_STATE state_;

  GilHelper() :
      state_(PyGILState_Ensure()) {
  }
  ~GilHelper() {
    PyGILState_Release(state_);
  }
};

RegisterFrame* Evaluator::frame_from_python(PyObject* function, PyObject* args) {
  GilHelper h;
  PyObject* globals = PyFunction_GetGlobals(function);
  PyCodeObject* code = (PyCodeObject*) PyFunction_GetCode(function);

  if (args == NULL || !PyTuple_Check(args)) {
    EVAL_LOG("Not a tuple? %s", obj_to_str(PyObject_Type(args)));
    return NULL;
  }

  PyFrameObject* frame = PyFrame_New(PyGILState_GetThisThreadState(), code, globals, NULL);
  PyObject* locals = PyDict_New();

  for (int i = 0; i < PyTuple_Size(args); ++i) {
    PyDict_SetItem(locals, PyTuple_GetItem(code->co_varnames, i), PyTuple_GetItem(args, i));
  }

  for (int i = 0; i < PyTuple_Size(code->co_cellvars); ++i) {
    PyDict_SetItem(locals, PyTuple_GetItem(code->co_cellvars, i), PyCell_New(NULL));
  }

  frame->f_locals = locals;

  PyObject* regcode = compileByteCode((PyCodeObject*) PyFunction_GetCode(function));
  if (!regcode) {
    return NULL;
  }
  return new RegisterFrame(frame, regcode);
}

RegisterFrame* Evaluator::frame_from_regcode(PyObject* regCode) {
  PyObject* locals = PyDict_New();
  PyObject* globals = PyEval_GetGlobals();
  PyCodeObject* code = PyCode_NewEmpty("*register code*", "*register function*", 0);
  PyFrameObject* frame = PyFrame_New(PyGILState_GetThisThreadState(), code, globals, NULL);
  PyFrame_FastToLocals(frame);
  frame->f_locals = locals;
  PyFrame_LocalsToFast(frame, 0);

  return new RegisterFrame(frame, regCode);
}

PyObject* Evaluator::eval_python(PyObject* func, PyObject* args) {
  RegisterFrame* frame = frame_from_python(func, args);
  if (!frame) {
    return NULL;
  }

  PyObject* result = eval(frame);
  delete frame;
  return result;
}

void Evaluator::dumpStatus() {
  Log_Info("Evaluator status:");
  Log_Info("%d operations executed.", totalCount);
  for (int i = 0; i < 256; ++i) {
    if (opCounts[i] > 0) {
      Log_Info("%20s : %10d, %.3f", OpUtil::name(i), opCounts[i], opTimes[i] / 1e9);
    }
  }
}

void Evaluator::collectInfo(int opcode) {
  ++totalCount;
  //  ++opCounts[opcode];
//    if (totalCount % 113 == 0) {
  //    opTimes[opcode] += rdtsc() - lastClock;
  //    lastClock = rdtsc();
  //  }
  if (totalCount > 1e9) {
    dumpStatus();
    throw new EvalError(PyExc_SystemError, "Execution entered infinite loop.");
  }
}

template<class OpType, class SubType>
struct Op {
};

template<class SubType>
struct Op<RegOp, SubType> {
  f_inline void eval(RunState* state, const char** pc, PyObject** registers) {
    RegOp op = *((RegOp*) *pc);
    *pc += sizeof(RegOp);
    ((SubType*) this)->_eval(op, registers, state);
  }

  static SubType instance;
};

template<class SubType>
SubType Op<RegOp, SubType>::instance;

template<class SubType>
struct Op<VarRegOp, SubType> {
  f_inline void eval(RunState* state, const char** pc, PyObject** registers) {
    VarRegOp *op = (VarRegOp*) (*pc);
    *pc += RMachineOp::size(*op);
    ((SubType*) this)->_eval(op, registers, state);
  }

  static SubType instance;
};
template<class SubType>
SubType Op<VarRegOp, SubType>::instance;

template<class SubType>
struct Op<BranchOp, SubType> {
  f_inline void eval(RunState* state, const char** pc, PyObject** registers) {
    BranchOp op = *((BranchOp*) *pc);
    ((SubType*) this)->_eval(op, pc, registers, state);
  }

  static SubType instance;
};

template<class SubType>
SubType Op<BranchOp, SubType>::instance;

struct IntegerOps {
#define _OP(name, op)\
  static f_inline PyObject* name(PyObject* w, PyObject* v) {\
    if (!PyInt_CheckExact(v) || !PyInt_CheckExact(w)) {\
      return NULL;\
    }\
    register long a, b, i;\
    a = PyInt_AS_LONG(v);\
    b = PyInt_AS_LONG(w);\
    i = (long) ((unsigned long) a op b);\
    if ((i ^ a) < 0 && (i ^ b) < 0) {\
      return NULL;\
    }\
    return PyInt_FromLong(i);\
  }

  _OP(add, +)
  _OP(sub, -)
  _OP(mul, *)
  _OP(div, /)
  _OP(mod, %)

  static f_inline PyObject* compare(PyObject* w, PyObject* v, int arg) {
    if (!PyInt_CheckExact(v) || !PyInt_CheckExact(w)) {
      return NULL;
    }

    long a = PyInt_AS_LONG(v);
    long b = PyInt_AS_LONG(w);

    switch (arg) {
    case PyCmp_LT:
      return a < b ? Py_True : Py_False ;
    case PyCmp_LE:
      return a <= b ? Py_True : Py_False ;
    case PyCmp_EQ:
      return a == b ? Py_True : Py_False ;
    case PyCmp_NE:
      return a != b ? Py_True : Py_False ;
    case PyCmp_GT:
      return a > b ? Py_True : Py_False ;
    case PyCmp_GE:
      return a >= b ? Py_True : Py_False ;
    case PyCmp_IS:
      return v == w ? Py_True : Py_False ;
    case PyCmp_IS_NOT:
      return v != w ? Py_True : Py_False ;
    default:
      return NULL;
    }

    return NULL;
  }
};

template<int OpCode, PyNumberFunction ObjF, PyNumberFunction IntegerF>
struct BinaryOpWithSpecialization: public Op<RegOp, BinaryOpWithSpecialization<OpCode, ObjF, IntegerF> > {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* r1 = registers[op.reg_1];
    PyObject* r2 = registers[op.reg_2];
    PyObject* r3 = NULL;
    r3 = IntegerF(r1, r2);
    if (r3 == NULL) {
      r3 = ObjF(r1, r2);
    }
    Py_XDECREF(registers[op.reg_3]);
    registers[op.reg_3] = r3;
  }
};

template<int OpCode, PyNumberFunction ObjF>
struct BinaryOp: public Op<RegOp, BinaryOp<OpCode, ObjF> > {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* r1 = registers[op.reg_1];
    PyObject* r2 = registers[op.reg_2];
    PyObject* r3 = ObjF(r1, r2);
    registers[op.reg_3] = r3;
  }
};

struct BinaryPower: public Op<RegOp, BinaryPower> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* r1 = registers[op.reg_1];
    PyObject* r2 = registers[op.reg_2];
    PyObject* r3 = PyNumber_Power(r1, r2, Py_None);
    registers[op.reg_3] = r3;
  }
};

struct InplacePower: public Op<RegOp, InplacePower> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* r1 = registers[op.reg_1];
    PyObject* r2 = registers[op.reg_2];
    PyObject* r3 = PyNumber_Power(r1, r2, Py_None);
    registers[op.reg_3] = r3;
  }
};


struct CompareOp: public Op<RegOp, CompareOp> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* r1 = registers[op.reg_1];
    PyObject* r2 = registers[op.reg_2];
    PyObject* r3 = IntegerOps::compare(r1, r2, op.arg);
    if (r3 != NULL) {
      Py_INCREF(r3);
    } else {
      r3 = PyObject_RichCompare(r2, r1, op.arg);
    }

    EVAL_LOG("Compare: %s, %s -> %s", obj_to_str(r1), obj_to_str(r2), obj_to_str(r3));
    Py_XDECREF(registers[op.reg_3]);
    registers[op.reg_3] = r3;
  }
};

struct IncRef: public Op<RegOp, IncRef> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    Py_INCREF(registers[op.reg_1]);
  }
};

struct DecRef: public Op<RegOp, DecRef> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    Py_DECREF(registers[op.reg_1]);
  }
};

struct LoadLocals: public Op<RegOp, LoadLocals> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    registers[op.reg_1] = state->frame->f_locals;
    Py_INCREF(registers[op.reg_1]);
  }
};

struct LoadGlobal: public Op<RegOp, LoadGlobal> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* r1 = PyTuple_GET_ITEM(state->names(), op.arg) ;
    PyObject* r2 = PyDict_GetItem(state->globals(), r1);
    if (r2 == NULL) {
      r2 = PyDict_GetItem(state->builtins(), r1);
    }
    if (r2 == NULL) {
      throw new EvalError(PyExc_NameError, "Global name %.200s not defined.", r1);
    }
    registers[op.reg_1] = r2;
  }
};

struct LoadName: public Op<RegOp, LoadName> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* r1 = PyTuple_GET_ITEM(state->names(), op.arg) ;
    PyObject* r2 = PyDict_GetItem(state->locals(), r1);
    if (r2 == NULL) {
      r2 = PyDict_GetItem(state->globals(), r1);
    }
    if (r2 == NULL) {
      r2 = PyDict_GetItem(state->frame->f_builtins, r1);
    }
    Py_INCREF(r2);
    registers[op.reg_1] = r2;
  }
};

struct LoadFast: public Op<RegOp, LoadFast> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    Py_INCREF(registers[op.reg_1]);
//    Py_XDECREF(registers[op.reg_2]);
    registers[op.reg_2] = registers[op.reg_1];
  }
};

struct StoreFast: public Op<RegOp, StoreFast> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    Py_XDECREF(registers[op.reg_2]);
    registers[op.reg_2] = registers[op.reg_1];
  }
};

struct StoreName: public Op<RegOp, StoreName> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* r1 = PyTuple_GET_ITEM(state->names(), op.arg) ;
    PyObject* r2 = registers[op.reg_1];
    PyObject_SetItem(state->frame->f_locals, r1, r2);
  }
};

struct StoreAttr: public Op<RegOp, StoreAttr> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* t = PyTuple_GET_ITEM(state->names(), op.arg) ;
    PyObject* key = registers[op.reg_1];
    PyObject* value = registers[op.reg_2];
    PyObject_SetAttr(t, key, value);
  }
};

struct StoreSubscr: public Op<RegOp, StoreSubscr> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* key = registers[op.reg_1];
    PyObject* list = registers[op.reg_2];
    PyObject* value = registers[op.reg_3];
    PyObject_SetItem(list, key, value);
  }
};

struct BinarySubscr: public Op<RegOp, BinarySubscr> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* key = registers[op.reg_1];
    PyObject* list = registers[op.reg_2];
    registers[op.reg_3] = PyObject_GetItem(list, key);

  }
};

struct ConstIndex: public Op<RegOp, ConstIndex> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* list = registers[op.reg_1];
    uint8_t key = op.arg;
    registers[op.reg_2] = PyObject_GetItem(list, PyInt_FromLong(key));
  }
};

struct LoadAttr: public Op<RegOp, LoadAttr> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* obj = registers[op.reg_1];
    PyObject* name = PyTuple_GET_ITEM(state->names(), op.arg) ;
    registers[op.reg_2] = PyObject_GetAttr(obj, name);
//    Py_DECREF(obj);
  }
};

struct CallFunction: public Op<VarRegOp, CallFunction> {
  f_inline void _eval(VarRegOp *op, PyObject** registers, RunState* state) {
    int na = op->arg & 0xff;
    int nk = (op->arg >> 8) & 0xff;
    int n = nk * 2 + na;
    int i;
    PyObject* fn = registers[op->regs[n]];

    assert(n + 2 == op->num_registers);

    if (state->call_args == NULL || PyTuple_GET_SIZE(state->call_args) != na) {
      Py_XDECREF(state->call_args);
      state->call_args = PyTuple_New(na);
    }

    PyObject* args = state->call_args;
    for (i = 0; i < na; ++i) {
      PyTuple_SET_ITEM(args, i, registers[op->regs[i]]);
    }

    PyObject* kwdict = NULL;
    if (nk > 0) {
      kwdict = PyDict_New();
      for (i = na; i < nk * 2; i += 2) {
        PyDict_SetItem(kwdict, registers[op->regs[i]], registers[op->regs[i + 1]]);
      }
    }

    PyObject* res = NULL;
    if (PyCFunction_Check(fn)) {
      res = PyCFunction_Call(fn, args, kwdict);
    } else {
      res = PyObject_Call(fn, args, kwdict);
    }

    if (res == NULL) {
      throw new EvalError();
    }

    registers[op->regs[n + 1]] = res;

  }
};

struct GetIter: public Op<RegOp, GetIter> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* res = PyObject_GetIter(registers[op.reg_1]);
    registers[op.reg_2] = res;
  }
};

struct ForIter: public Op<BranchOp, ForIter> {
  f_inline void _eval(BranchOp op, const char **pc, PyObject** registers, RunState *state) {
    PyObject* r1 = PyIter_Next(registers[op.reg_1]);
    if (r1) {
      Py_XDECREF(registers[op.reg_2]);
      registers[op.reg_2] = r1;
      *pc += sizeof(BranchOp);
    } else {
      *pc = state->code + op.label;
    }

  }
};

struct JumpIfFalseOrPop: public Op<BranchOp, JumpIfFalseOrPop> {
  f_inline void _eval(BranchOp op, const char **pc, PyObject** registers, RunState *state) {
    PyObject *r1 = registers[op.reg_1];
    if (r1 == Py_False || (PyObject_IsTrue(r1) == 0)) {
//      EVAL_LOG("Jumping: %s -> %d", obj_to_str(r1), op.label);
      *pc = state->code + op.label;
    } else {
      *pc += sizeof(BranchOp);
    }

  }
};

struct JumpIfTrueOrPop: public Op<BranchOp, JumpIfTrueOrPop> {
  f_inline void _eval(BranchOp op, const char **pc, PyObject** registers, RunState *state) {
    PyObject* r1 = registers[op.reg_1];
    if (r1 == Py_True || (PyObject_IsTrue(r1) == 1)) {
      *pc = state->code + op.label;
    } else {
      *pc += sizeof(BranchOp);
    }

  }
};

struct JumpAbsolute: public Op<BranchOp, JumpAbsolute> {
  f_inline void _eval(BranchOp op, const char **pc, PyObject** registers, RunState *state) {
    EVAL_LOG("Jumping to: %d", op.label);
    *pc = state->code + op.label;
  }
};

struct ReturnValue: public Op<RegOp, ReturnValue> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* result = registers[op.reg_1];
    Py_INCREF(result);
    throw result;
  }
};

struct PopBlock: public Op<RegOp, PopBlock> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {

  }
};

struct Nop: public Op<RegOp, Nop> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {

  }
};

struct BuildTuple: public Op<VarRegOp, BuildTuple> {
  f_inline void _eval(VarRegOp *op, PyObject** registers, RunState* state) {
    int i;
    PyObject* t = PyTuple_New(op->arg);
    for (i = 0; i < op->arg; ++i) {
      PyTuple_SET_ITEM(t, i, registers[op->regs[i]]);
    }
    registers[op->regs[op->arg]] = t;

  }
};

struct BuildList: public Op<VarRegOp, BuildList> {
  f_inline void _eval(VarRegOp *op, PyObject** registers, RunState* state) {
    int i;
    PyObject* t = PyList_New(op->arg);
    for (i = 0; i < op->arg; ++i) {
      PyList_SET_ITEM(t, i, registers[op->regs[i]]);
    }
    registers[op->regs[op->arg]] = t;
  }
};

struct PrintItem: public Op<RegOp, PrintItem> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* v = registers[op.reg_1];
    PyObject* w = op.reg_2 >= 0 ? registers[op.reg_2] : PySys_GetObject("stdout");

    int err = 0;
    if (w != NULL && PyFile_SoftSpace(w, 0)) {
      err = PyFile_WriteString(" ", w);
    }
    if (err == 0) {
      err = PyFile_WriteObject(v, w, Py_PRINT_RAW);
    }
    if (err == 0) {
      /* XXX move into writeobject() ? */
      if (PyString_Check(v)) {
        char *s = PyString_AS_STRING(v);
        Py_ssize_t len = PyString_GET_SIZE(v);
        if (len == 0 || !isspace(Py_CHARMASK(s[len-1]) ) || s[len - 1] == ' ') PyFile_SoftSpace(w, 1);
      }
#ifdef Py_USING_UNICODE
      else if (PyUnicode_Check(v)) {
        Py_UNICODE *s = PyUnicode_AS_UNICODE(v);
        Py_ssize_t len = PyUnicode_GET_SIZE(v);
        if (len == 0 || !Py_UNICODE_ISSPACE(s[len-1]) || s[len - 1] == ' ') PyFile_SoftSpace(w, 1);
      }
#endif
      else PyFile_SoftSpace(w, 1);
    }
  }
};

struct PrintNewline: public Op<RegOp, PrintNewline> {
  f_inline void _eval(RegOp op, PyObject** registers, RunState* state) {
    PyObject* w = op.reg_2 >= 0 ? registers[op.reg_2] : PySys_GetObject("stdout");
    int err = PyFile_WriteString("\n", w);
    if (err == 0) PyFile_SoftSpace(w, 0);
  }
};

#define CONCAT(...) __VA_ARGS__

#define REGISTER_OP(opname)\
    static int _force_register_ ## opname = LabelRegistry::add_label(opname, &&op_ ## opname);

#define _DEFINE_OP(opname, impl)\
      EVAL_LOG("%5d: %s", state.offset(pc), OpUtil::name(opname));\
      /*collectInfo(opname);\*/\
      impl::instance.eval(&state, &pc, registers);\
      goto *labels[state.next_code(pc)];

#define DEFINE_OP(opname, impl)\
    op_##opname:\
      _DEFINE_OP(opname, impl)

#define _BAD_OP(opname)\
        { EVAL_LOG("Not implemented: %s", #opname); throw new EvalError(PyExc_SystemError, "Bad opcode %s", #opname); }

#define BAD_OP(opname)\
    op_##opname: _BAD_OP(opname)

#define FALLTHROUGH(opname) op_##opname:

#define INTEGER_OP(op)\
    a = PyInt_AS_LONG(v);\
    b = PyInt_AS_LONG(w);\
    i = (long)((unsigned long)a op b);\
    if ((i^a) < 0 && (i^b) < 0)\
        goto slow_path;\
    x = PyInt_FromLong(i);

#define BINARY_OP3(opname, objfn, intfn)\
    op_##opname:\
      _DEFINE_OP(opname, BinaryOpWithSpecialization<CONCAT(opname, objfn, intfn)>)

#define BINARY_OP2(opname, objfn)\
    op_##opname:\
      _DEFINE_OP(opname, BinaryOp<CONCAT(opname, objfn)>)

PyObject* Evaluator::eval(RegisterFrame *reg_frame) {
  if (reg_frame == NULL) {
    PyErr_SetString(PyExc_SystemError, "Invalid frame.");
    return NULL;
  }

  RunState state(reg_frame);

  register const char* pc = state.code + sizeof(RegisterPrelude);
  register PyObject* registers[state.prelude.num_registers];

  PyObject* locals = state.locals();
  PyObject* consts = state.consts();
  PyCodeObject* pycode = state.frame->f_code;

  int num_consts = PyTuple_Size(consts);
// int num_locals = state.frame->f_code->co_nlocals;

// setup const and local register aliases.
  for (int i = 0; i < state.prelude.num_registers; ++i) {
    registers[i] = NULL;
  }

  for (int i = 0; i < num_consts; ++i) {
    registers[i] = PyTuple_GET_ITEM(consts, i) ;
  }

  int offset = 0;
  for (int i = 0; i < PyTuple_Size(pycode->co_varnames); ++i) {
    registers[num_consts + offset] = PyDict_GetItem(locals, PyTuple_GetItem(pycode->co_varnames, i));
    ++offset;
  }

  for (int i = 0; i < PyTuple_Size(pycode->co_cellvars); ++i) {
    registers[num_consts + offset] = PyDict_GetItem(locals, PyTuple_GetItem(pycode->co_cellvars, i));
    ++offset;
  }

  lastClock = rdtsc();

const void* const labels[] = {
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
  &&op_DECREF,
  &&op_CONST_INDEX
}
;

EVAL_LOG("Jumping to: %d, %s", state.offset(pc), OpUtil::name(state.next_code(pc)));
try {
    goto *labels[state.next_code(pc)];

BINARY_OP3(BINARY_MULTIPLY, PyNumber_Multiply, IntegerOps::mul);
BINARY_OP3(BINARY_DIVIDE, PyNumber_Divide, IntegerOps::div);
BINARY_OP3(BINARY_ADD, PyNumber_Add, IntegerOps::add);
BINARY_OP3(BINARY_SUBTRACT, PyNumber_Subtract, IntegerOps::sub);
BINARY_OP3(BINARY_MODULO, PyNumber_Remainder, IntegerOps::mod);

BINARY_OP2(BINARY_OR, PyNumber_Or);
BINARY_OP2(BINARY_XOR, PyNumber_Xor);
BINARY_OP2(BINARY_AND, PyNumber_And);
BINARY_OP2(BINARY_RSHIFT, PyNumber_Rshift);
BINARY_OP2(BINARY_LSHIFT, PyNumber_Lshift);
BINARY_OP2(BINARY_TRUE_DIVIDE, PyNumber_TrueDivide);
BINARY_OP2(BINARY_FLOOR_DIVIDE, PyNumber_FloorDivide);
DEFINE_OP(BINARY_POWER, BinaryPower);

BINARY_OP3(INPLACE_MULTIPLY, PyNumber_InPlaceMultiply, IntegerOps::mul);
BINARY_OP3(INPLACE_DIVIDE, PyNumber_InPlaceDivide, IntegerOps::div);
BINARY_OP3(INPLACE_ADD, PyNumber_InPlaceAdd, IntegerOps::add);
BINARY_OP3(INPLACE_SUBTRACT, PyNumber_InPlaceSubtract, IntegerOps::sub);
BINARY_OP3(INPLACE_MODULO, PyNumber_InPlaceRemainder, IntegerOps::mod);

BINARY_OP2(INPLACE_OR, PyNumber_InPlaceOr);
BINARY_OP2(INPLACE_XOR, PyNumber_InPlaceXor);
BINARY_OP2(INPLACE_AND, PyNumber_InPlaceAnd);
BINARY_OP2(INPLACE_RSHIFT, PyNumber_InPlaceRshift);
BINARY_OP2(INPLACE_LSHIFT, PyNumber_InPlaceLshift);
BINARY_OP2(INPLACE_TRUE_DIVIDE, PyNumber_InPlaceTrueDivide);
BINARY_OP2(INPLACE_FLOOR_DIVIDE, PyNumber_InPlaceFloorDivide);
DEFINE_OP(INPLACE_POWER, InplacePower);


DEFINE_OP(LOAD_FAST, LoadFast);
DEFINE_OP(LOAD_LOCALS, LoadLocals);
DEFINE_OP(LOAD_GLOBAL, LoadGlobal);
DEFINE_OP(LOAD_NAME, LoadName);
DEFINE_OP(LOAD_ATTR, LoadAttr);

DEFINE_OP(STORE_NAME, StoreName);
DEFINE_OP(STORE_ATTR, StoreAttr);
DEFINE_OP(STORE_SUBSCR, StoreSubscr);
DEFINE_OP(STORE_FAST, StoreFast);

DEFINE_OP(BINARY_SUBSCR, BinarySubscr);
DEFINE_OP(CONST_INDEX, ConstIndex);

DEFINE_OP(GET_ITER, GetIter);
DEFINE_OP(FOR_ITER, ForIter);
DEFINE_OP(RETURN_VALUE, ReturnValue);

DEFINE_OP(BUILD_TUPLE, BuildTuple);
DEFINE_OP(BUILD_LIST, BuildList);

DEFINE_OP(PRINT_NEWLINE, PrintNewline);
DEFINE_OP(PRINT_NEWLINE_TO, PrintNewline);
DEFINE_OP(PRINT_ITEM, PrintItem);
DEFINE_OP(PRINT_ITEM_TO, PrintItem);


FALLTHROUGH(CALL_FUNCTION);
FALLTHROUGH(CALL_FUNCTION_VAR);
FALLTHROUGH(CALL_FUNCTION_KW);
DEFINE_OP(CALL_FUNCTION_VAR_KW, CallFunction);

FALLTHROUGH(POP_JUMP_IF_FALSE);
DEFINE_OP(JUMP_IF_FALSE_OR_POP, JumpIfFalseOrPop);

FALLTHROUGH(POP_JUMP_IF_TRUE);
DEFINE_OP(JUMP_IF_TRUE_OR_POP, JumpIfTrueOrPop);

DEFINE_OP(JUMP_ABSOLUTE, JumpAbsolute);

FALLTHROUGH(SETUP_LOOP);
DEFINE_OP(POP_BLOCK, PopBlock);

DEFINE_OP(COMPARE_OP, CompareOp);
DEFINE_OP(INCREF, IncRef);
DEFINE_OP(DECREF, DecRef);

BAD_OP(LOAD_CONST);
BAD_OP(JUMP_FORWARD);
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
BAD_OP(PRINT_EXPR);
BAD_OP(DELETE_SUBSCR);
BAD_OP(STORE_MAP);
BAD_OP(DELETE_SLICE);
BAD_OP(STORE_SLICE);
BAD_OP(SLICE);
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
op_BADCODE: {
      EVAL_LOG("Jump to invalid opcode!?");
throw new EvalError(PyExc_SystemError, "Invalid jump.");
    }
  } catch (PyObject* result) {
    return result;
  } catch (EvalError *error) {
    if (!PyErr_Occurred()) {
      PyErr_SetObject(error->exception, error->value);
    }
    delete error;
    return NULL;
  }
  Log_Fatal("Shouldn't reach here.");
return NULL;
}
