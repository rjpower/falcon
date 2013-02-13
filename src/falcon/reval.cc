#include <Python.h>
#include <opcode.h>
#include <marshal.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "reval.h"
#include "rcompile.h"

#ifdef FALCON_DEBUG
static bool logging_enabled() {
  static bool _is_logging = getenv("EVAL_LOG") != NULL;
  return _is_logging;
}

#define EVAL_LOG(...) if (logging_enabled()) { fprintf(stderr, __VA_ARGS__); fputs("\n", stderr); }
#define CHECK_VALID(obj) { Reg_AssertGt(obj->ob_refcnt, 0); }
#else
#define EVAL_LOG(...)
#define CHECK_VALID(obj) { }
//#define CHECK_VALID(obj) Reg_AssertGt(obj->ob_refcnt, 0);
#endif

typedef PyObject* (*BinaryFunction)(PyObject*, PyObject*);
typedef PyObject* (*UnaryFunction)(PyObject*);

struct GilHelper {
  PyGILState_STATE state_;

  GilHelper() :
      state_(PyGILState_Ensure()) {
  }
  ~GilHelper() {
    PyGILState_Release(state_);
  }
};

struct RefHelper {
  PyObject* obj;f_inline RefHelper(PyObject* o) :
      obj(o) {
    Py_INCREF(obj);
  }
  f_inline ~RefHelper() {
    Py_DECREF(obj);
  }
};

// Catch bad case of RefHelper(x) (goes immediately out of scope
#define GilHelper(v) static int MustInitWithVar[-1];
#define RefHelper(v) static int MustInitWithVar[-1];

RegisterFrame::RegisterFrame(RegisterCode* func, PyObject* obj, const ObjVector* args, const ObjVector* kw) :
    code(func) {
  instructions = code->instructions.data();

  globals_ = PyFunction_GetGlobals(func->function);
  builtins_ = PyEval_GetBuiltins();

  py_call_args = NULL;
  locals_ = NULL;

  registers = new PyObject*[func->num_registers];

  const int num_registers = code->num_registers;

  // setup const and local register aliases.
  for (int i = 0; i < num_registers; ++i) {
    registers[i] = NULL;
  }

  int num_consts = PyTuple_GET_SIZE(consts());

  for (int i = 0; i < num_consts; ++i) {
    registers[i] = PyTuple_GET_ITEM(consts(), i) ;
    Py_INCREF(registers[i]);
  }

  int needed_args = code->code()->co_argcount;
  int offset = num_consts;
  if (PyMethod_Check(obj)) {
    PyObject* klass = PyMethod_GET_CLASS(obj);
    PyObject* self = PyMethod_GET_SELF(obj);

    Reg_Assert(self != NULL, "Method call without a bound self.");
    registers[offset++] = self;
    Py_INCREF(self);

    needed_args--;
  }

  PyObject* def_args = PyFunction_GET_DEFAULTS(code->function);
  int num_def_args = def_args == NULL ? 0 : PyTuple_GET_SIZE(def_args);
  int num_args = args->size();
  if (num_args + num_def_args < needed_args) {
    throw RException(PyExc_TypeError, "Wrong number of arguments for %s, expected %d, got %d.",
                     PyEval_GetFuncName(code->function), needed_args - num_def_args, num_args);
  }

  for (int i = 0; i < needed_args; ++i) {
    PyObject* v = (i < num_args) ? args->at(i) : PyTuple_GET_ITEM(def_args, i - num_args) ;
    Py_INCREF(v);
    registers[offset++] = v;
  }
}

RegisterFrame::~RegisterFrame() {
  Py_XDECREF(py_call_args);

  const int num_registers = code->num_registers;
  register PyObject** r = registers;
  for (register int i = 0; i < num_registers; ++i) {
    Py_XDECREF(r[i]);
  }

  delete[] r;
}

Evaluator::Evaluator() {
  bzero(op_counts_, sizeof(op_counts_));
  bzero(op_times_, sizeof(op_times_));
  total_count_ = 0;
  last_clock_ = 0;
  compiler_ = new Compiler;
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

RegisterCode* Evaluator::compile(PyObject* obj) {
  return compiler_->compile(obj);
}

RegisterFrame* Evaluator::frame_from_python(PyObject* obj, PyObject* args) {
  if (args == NULL || !PyTuple_Check(args)) {
    throw RException(PyExc_TypeError, "Expected function argument tuple, got: %s", obj_to_str(PyObject_Type(args)));
  }

  RegisterCode* regcode = compile(obj);
  if (!regcode) {
    return NULL;
  }

  ObjVector v_args;
  for (int i = 0; i < PyTuple_GET_SIZE(args) ; ++i) {
    v_args.push_back(PyTuple_GET_ITEM(args, i) );
  }

  return new RegisterFrame(regcode, obj, &v_args, NULL);
}

void Evaluator::dump_status() {
  Log_Info("Evaluator status:");
  Log_Info("%d operations executed.", total_count_);
  for (int i = 0; i < 256; ++i) {
    if (op_counts_[i] > 0) {
      Log_Info("%20s : %10d, %.3f", OpUtil::name(i), op_counts_[i], op_times_[i] / 1e9);
    }
  }
}

void Evaluator::collect_info(int opcode) {
  ++total_count_;
  //  ++op_counts_[opcode];
//    if (total_count_ % 113 == 0) {
  //    op_times_[opcode] += rdtsc() - last_clock_;
  //    last_clock_ = rdtsc();
  //  }
  if (total_count_ > 1e9) {
    dump_status();
    throw RException(PyExc_SystemError, "Execution entered infinite loop.");
  }
}
template<class OpType, class SubType>
struct RegOpImpl {
  static f_inline void eval(Evaluator* eval, RegisterFrame* state, const char** pc, PyObject** registers) {
    OpType op = *((OpType*) *pc);
    EVAL_LOG("%5d: %s", state->offset(*pc), op.str().c_str());
    *pc += op.size();
    SubType::_eval(eval, state, op, registers);
  }
};

template<class SubType>
struct VarArgsOpImpl {
  static f_inline void eval(Evaluator* eval, RegisterFrame* state, const char** pc, PyObject** registers) {
    VarRegOp *op = (VarRegOp*) (*pc);
    EVAL_LOG("%5d: %s", state->offset(*pc), op->str().c_str());
    *pc += op->size();
    SubType::_eval(eval, state, op, registers);
  }
};

template<class SubType>
struct BranchOpImpl {
  static f_inline void eval(Evaluator* eval, RegisterFrame* state, const char** pc, PyObject** registers) {
    BranchOp op = *((BranchOp*) *pc);
    EVAL_LOG("%5d: %s", state->offset(*pc), op.str().c_str());
    SubType::_eval(eval, state, op, pc, registers);
  }
};

struct IntegerOps {
#define _OP(name, op)\
  static f_inline PyObject* name(PyObject* w, PyObject* v) {\
    if (!PyInt_CheckExact(v) || !PyInt_CheckExact(w)) {\
      return NULL;\
    }\
    register long a, b, i;\
    a = PyInt_AS_LONG(w);\
    b = PyInt_AS_LONG(v);\
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

    long a = PyInt_AS_LONG(w);
    long b = PyInt_AS_LONG(v);

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

struct FloatOps {
  static f_inline PyObject* compare(PyObject* w, PyObject* v, int arg) {
    if (!PyFloat_CheckExact(v) || !PyFloat_CheckExact(w)) {
      return NULL;
    }

    double a = PyFloat_AsDouble(w);
    double b = PyFloat_AsDouble(v);

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

template<int OpCode, BinaryFunction ObjF, BinaryFunction IntegerF>
struct BinaryOpWithSpecialization: public RegOpImpl<RegOp<3>, BinaryOpWithSpecialization<OpCode, ObjF, IntegerF> > {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<3> op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    PyObject* r2 = registers[op.reg[1]];
    PyObject* r3 = NULL;
    r3 = IntegerF(r1, r2);
    if (r3 == NULL) {
      r3 = ObjF(r1, r2);
    }
    Py_XDECREF(registers[op.reg[2]]);
    registers[op.reg[2]] = r3;
  }
};

template<int OpCode, BinaryFunction ObjF>
struct BinaryOp: public RegOpImpl<RegOp<3>, BinaryOp<OpCode, ObjF> > {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<3> op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    PyObject* r2 = registers[op.reg[1]];
    CHECK_VALID(r1);
    CHECK_VALID(r2);
    PyObject* r3 = ObjF(r1, r2);
    CHECK_VALID(r3);
    Py_XDECREF(registers[op.reg[2]]);
    registers[op.reg[2]] = r3;
  }
};

template<int OpCode, UnaryFunction ObjF>
struct UnaryOp: public RegOpImpl<RegOp<2>, UnaryOp<OpCode, ObjF> > {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<2> op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    CHECK_VALID(r1);
    PyObject* r2 = ObjF(r1);
    CHECK_VALID(r2);
    Py_XDECREF(registers[op.reg[1]]);
    registers[op.reg[1]] = r2;
  }
};

struct UnaryNot: public RegOpImpl<RegOp<2>, UnaryNot> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<2> op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    PyObject* res = PyObject_IsTrue(r1) ? Py_False : Py_True;
    Py_INCREF(res);
    Py_XDECREF(registers[op.reg[1]]);
    registers[op.reg[1]] = res;
  }
};

struct BinaryPower: public RegOpImpl<RegOp<3>, BinaryPower> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<3> op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    CHECK_VALID(r1);
    PyObject* r2 = registers[op.reg[1]];
    CHECK_VALID(r2);
    PyObject* r3 = PyNumber_Power(r1, r2, Py_None);
    CHECK_VALID(r3);
    Py_XDECREF(registers[op.reg[1]]);
    registers[op.reg[2]] = r3;
  }
};

struct BinarySubscr: public RegOpImpl<RegOp<3>, BinarySubscr> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<3> op, PyObject** registers) {
    PyObject* list = registers[op.reg[0]];
    PyObject* key = registers[op.reg[1]];
    CHECK_VALID(list);
    CHECK_VALID(key);
    PyObject* res = NULL;
    if (PyList_CheckExact(list) && PyInt_CheckExact(key)) {
      Py_ssize_t i = PyInt_AsSsize_t(key);
      if (i < 0) i += PyList_GET_SIZE(list);
      if (i >= 0 && i < PyList_GET_SIZE(list) ) {
        res = PyList_GET_ITEM(list, i) ;
        Py_INCREF(res);
      }
    }
    if (!res) {
      res = PyObject_GetItem(list, key);
    }
    if (!res) {
      throw RException();
    }

    CHECK_VALID(res);

    Py_XDECREF(registers[op.reg[2]]);
    registers[op.reg[2]] = res;
  }
};

struct InplacePower: public RegOpImpl<RegOp<3>, InplacePower> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<3> op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    CHECK_VALID(r1);
    PyObject* r2 = registers[op.reg[1]];
    CHECK_VALID(r2);
    PyObject* r3 = PyNumber_Power(r1, r2, Py_None);
    Py_XDECREF(registers[op.reg[1]]);
    CHECK_VALID(r3);
    registers[op.reg[2]] = r3;
  }
};

struct CompareOp: public RegOpImpl<RegOp<3>, CompareOp> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<3> op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    CHECK_VALID(r1);
    PyObject* r2 = registers[op.reg[1]];
    CHECK_VALID(r2);
    PyObject* r3 = IntegerOps::compare(r1, r2, op.arg);
    if (r3 == NULL) {
      r3 = FloatOps::compare(r1, r2, op.arg);
    }

    if (r3 != NULL) {
      Py_INCREF(r3);
    } else {
      r3 = PyObject_RichCompare(r1, r2, op.arg);
    }
    CHECK_VALID(r3);

    EVAL_LOG("Compare: %s, %s -> %s", obj_to_str(r1), obj_to_str(r2), obj_to_str(r3));
    Py_XDECREF(registers[op.reg[2]]);
    registers[op.reg[2]] = r3;
  }
};

struct IncRef: public RegOpImpl<RegOp<1>, IncRef> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<1> op, PyObject** registers) {
    CHECK_VALID(registers[op.reg[0]]);
    Py_INCREF(registers[op.reg[0]]);
  }
};

struct DecRef: public RegOpImpl<RegOp<1>, DecRef> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<1> op, PyObject** registers) {
    CHECK_VALID(registers[op.reg[0]]);
    Py_DECREF(registers[op.reg[0]]);
  }
};

struct LoadLocals: public RegOpImpl<RegOp<1>, LoadLocals> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<1> op, PyObject** registers) {
    Py_XDECREF(registers[op.reg[0]]);
    Py_INCREF(state->locals());
    registers[op.reg[0]] = state->locals();
  }
};

struct LoadGlobal: public RegOpImpl<RegOp<1>, LoadGlobal> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<1> op, PyObject** registers) {
    PyObject* r1 = PyTuple_GET_ITEM(state->names(), op.arg) ;
    PyObject* r2 = PyDict_GetItem(state->globals(), r1);
    if (r2 == NULL) {
      r2 = PyDict_GetItem(state->builtins(), r1);
    }
    if (r2 == NULL) {
      throw RException(PyExc_NameError, "Global name %.200s not defined.", r1);
    }
    Py_INCREF(r2);
    Py_XDECREF(registers[op.reg[0]]);
    CHECK_VALID(r2);
    registers[op.reg[0]] = r2;
  }
};

struct LoadName: public RegOpImpl<RegOp<1>, LoadName> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<1> op, PyObject** registers) {
    PyObject* r1 = PyTuple_GET_ITEM(state->names(), op.arg) ;
    PyObject* r2 = PyDict_GetItem(state->locals(), r1);
    if (r2 == NULL) {
      r2 = PyDict_GetItem(state->globals(), r1);
    }
    if (r2 == NULL) {
      r2 = PyDict_GetItem(state->builtins(), r1);
    }
    if (r2 == NULL) {
      throw RException(PyExc_NameError, "Name %.200s not defined.", r1);
    }
    Py_INCREF(r2);
    Py_XDECREF(registers[op.reg[0]]);
    CHECK_VALID(r2);
    registers[op.reg[0]] = r2;
  }
};

struct LoadFast: public RegOpImpl<RegOp<2>, LoadFast> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<2> op, PyObject** registers) {
    Py_INCREF(registers[op.reg[0]]);
    Py_XDECREF(registers[op.reg[1]]);
    CHECK_VALID(registers[op.reg[0]]);
    registers[op.reg[1]] = registers[op.reg[0]];
  }
};

struct StoreFast: public RegOpImpl<RegOp<2>, StoreFast> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<2> op, PyObject** registers) {
    Py_XDECREF(registers[op.reg[1]]);
    Py_INCREF(registers[op.reg[0]]);
    CHECK_VALID(registers[op.reg[0]]);
    registers[op.reg[1]] = registers[op.reg[0]];
  }
};

struct StoreName: public RegOpImpl<RegOp<1>, StoreName> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<1> op, PyObject** registers) {
    PyObject* r1 = PyTuple_GET_ITEM(state->names(), op.arg) ;
    PyObject* r2 = registers[op.reg[0]];
    CHECK_VALID(r1);
    CHECK_VALID(r2);
    PyObject_SetItem(state->locals(), r1, r2);
  }
};

struct StoreAttr: public RegOpImpl<RegOp<2>, StoreAttr> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<2> op, PyObject** registers) {
    PyObject* t = PyTuple_GET_ITEM(state->names(), op.arg) ;
    PyObject* key = registers[op.reg[0]];
    PyObject* value = registers[op.reg[1]];
    CHECK_VALID(t);
    CHECK_VALID(key);
    CHECK_VALID(value);
    PyObject_SetAttr(t, key, value);
  }
};

struct StoreSubscr: public RegOpImpl<RegOp<3>, StoreSubscr> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<3> op, PyObject** registers) {
    PyObject* key = registers[op.reg[0]];
    PyObject* list = registers[op.reg[1]];
    PyObject* value = registers[op.reg[2]];
    CHECK_VALID(key);
    CHECK_VALID(list);
    CHECK_VALID(value);
    PyObject_SetItem(list, key, value);
  }
};

struct ConstIndex: public RegOpImpl<RegOp<2>, ConstIndex> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<2> op, PyObject** registers) {
    PyObject* list = registers[op.reg[0]];
    uint8_t key = op.arg;
    if (op.reg[1] == kInvalidRegister) {
      return;
    }
    Py_XDECREF(registers[op.reg[1]]);
    PyObject* pykey = PyInt_FromLong(key);
    registers[op.reg[1]] = PyObject_GetItem(list, pykey);
    Py_DECREF(pykey);
    Py_INCREF(registers[op.reg[1]]);
    CHECK_VALID(registers[op.reg[1]]);
  }
};

struct LoadAttr: public RegOpImpl<RegOp<2>, LoadAttr> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<2> op, PyObject** registers) {
    PyObject* obj = registers[op.reg[0]];
    PyObject* name = PyTuple_GET_ITEM(state->names(), op.arg) ;
    Py_XDECREF(registers[op.reg[1]]);
    registers[op.reg[1]] = PyObject_GetAttr(obj, name);
    CHECK_VALID(registers[op.reg[1]]);
//    Py_INCREF(registers[op.reg[1]]);
  }
};

struct CallFunction: public VarArgsOpImpl<CallFunction> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* state, VarRegOp *op, PyObject** registers) {
    int na = op->arg & 0xff;
    int nk = (op->arg >> 8) & 0xff;
    int n = nk * 2 + na;
    int i;
    PyObject* fn = registers[op->regs[n]];
    assert(n + 2 == op->num_registers);

    PyObject* res = NULL;
    RegisterCode* code = NULL;
    if (!PyCFunction_Check(fn)) {
      code = eval->compile(fn);
    }

    if (code == NULL || nk > 0) {
      if (state->py_call_args == NULL || PyTuple_GET_SIZE(state->py_call_args) != na) {
        Py_XDECREF(state->py_call_args);
        state->py_call_args = PyTuple_New(na);
      }

      PyObject* args = state->py_call_args;
      for (i = 0; i < na; ++i) {
        CHECK_VALID(registers[op->regs[i]]);
        Py_INCREF(registers[op->regs[i]]);
        PyTuple_SET_ITEM(args, i, registers[op->regs[i]]);
      }

      PyObject* kwdict = NULL;
      if (nk > 0) {
        kwdict = PyDict_New();
        for (i = na; i < nk * 2; i += 2) {
          CHECK_VALID(registers[op->regs[i]]);
          CHECK_VALID(registers[op->regs[i+i]]);
          Py_INCREF(registers[op->regs[i]]);
          Py_INCREF(registers[op->regs[i+1]]);
          PyDict_SetItem(kwdict, registers[op->regs[i]], registers[op->regs[i + 1]]);
        }
      }
      if (PyCFunction_Check(fn)) {
        res = PyCFunction_Call(fn, args, kwdict);
      } else {
        res = PyObject_Call(fn, args, kwdict);
      }
    } else {
      ObjVector& args = state->reg_call_args;
      args.resize(na);
      for (i = 0; i < na; ++i) {
        CHECK_VALID(registers[op->regs[i]]);
        args[i] = registers[op->regs[i]];
      }
      RegisterFrame f(code, fn, &args, NULL);
      res = eval->eval(&f);
    }

    if (res == NULL) {
      throw RException();
    }

    int dst = op->regs[n + 1];
    if (dst != kInvalidRegister) {
      Py_XDECREF(registers[dst]);
      registers[dst] = res;
    }
  }
};

struct GetIter: public RegOpImpl<RegOp<2>, GetIter> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<2> op, PyObject** registers) {
    PyObject* res = PyObject_GetIter(registers[op.reg[0]]);
    Py_XDECREF(registers[op.reg[1]]);
    registers[op.reg[1]] = res;
  }
};

struct ForIter: public BranchOpImpl<ForIter> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *state, BranchOp op, const char **pc,
                             PyObject** registers) {
    CHECK_VALID(registers[op.reg[0]]);
    PyObject* r1 = PyIter_Next(registers[op.reg[0]]);
    if (r1) {
      Py_XDECREF(registers[op.reg[1]]);
      registers[op.reg[1]] = r1;
      *pc += sizeof(BranchOp);
    } else {
      *pc = state->instructions + op.label;
    }

  }
};

struct JumpIfFalseOrPop: public BranchOpImpl<JumpIfFalseOrPop> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *state, BranchOp op, const char **pc,
                             PyObject** registers) {
    PyObject *r1 = registers[op.reg[0]];
    if (r1 == Py_False || (PyObject_IsTrue(r1) == 0)) {
//      EVAL_LOG("Jumping: %s -> %d", obj_to_str(r1), op.label);
      *pc = state->instructions + op.label;
    } else {
      *pc += sizeof(BranchOp);
    }

  }
};

struct JumpIfTrueOrPop: public BranchOpImpl<JumpIfTrueOrPop> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *state, BranchOp op, const char **pc,
                             PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    if (r1 == Py_True || (PyObject_IsTrue(r1) == 1)) {
      *pc = state->instructions + op.label;
    } else {
      *pc += sizeof(BranchOp);
    }

  }
};

struct JumpAbsolute: public BranchOpImpl<JumpAbsolute> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *state, BranchOp op, const char **pc,
                             PyObject** registers) {
    EVAL_LOG("Jumping to: %d", op.label);
    *pc = state->instructions + op.label;
  }
};

// Evaluation of RETURN_VALUE is special.  g++ exceptions are excrutiatingly slow, so we
// can't use the exception mechanism to jump to our exit point.  Instead, we return a value
// here and jump to the exit of our frame.
struct ReturnValue {
  static f_inline PyObject* eval(Evaluator* eval, RegisterFrame* state, const char** pc, PyObject** registers) {
    RegOp<1> op = *((RegOp<1>*) *pc);
    EVAL_LOG("%5d: %s", state->offset(*pc), op.str().c_str());
    PyObject* result = registers[op.reg[0]];
    Py_INCREF(result);
    return result;
  }
};

struct Nop: public RegOpImpl<RegOp<0>, Nop> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<0> op, PyObject** registers) {

  }
};

struct BuildTuple: public VarArgsOpImpl<BuildTuple> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* state, VarRegOp *op, PyObject** registers) {
    int i;
    PyObject* t = PyTuple_New(op->arg);
    for (i = 0; i < op->arg; ++i) {
      PyTuple_SET_ITEM(t, i, registers[op->regs[i]]);
    }
    registers[op->regs[op->arg]] = t;

  }
};

struct BuildList: public VarArgsOpImpl<BuildList> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* state, VarRegOp *op, PyObject** registers) {
    int i;
    PyObject* t = PyList_New(op->arg);
    for (i = 0; i < op->arg; ++i) {
      PyList_SET_ITEM(t, i, registers[op->regs[i]]);
    }
    registers[op->regs[op->arg]] = t;
  }
};

struct PrintItem: public RegOpImpl<RegOp<2>, PrintItem> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<2> op, PyObject** registers) {
    PyObject* v = registers[op.reg[0]];
    PyObject* w = op.reg[1] != kInvalidRegister ? registers[op.reg[1]] : PySys_GetObject((char*) "stdout");

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

struct PrintNewline: public RegOpImpl<RegOp<1>, PrintNewline> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<1> op, PyObject** registers) {
    PyObject* w = op.reg[0] != kInvalidRegister ? registers[op.reg[0]] : PySys_GetObject((char*) "stdout");
    int err = PyFile_WriteString("\n", w);
    if (err == 0) PyFile_SoftSpace(w, 0);
  }
};

struct ListAppend: public RegOpImpl<RegOp<2>, ListAppend> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<2> op, PyObject** registers) {
    PyList_Append(registers[op.reg[0]], registers[op.reg[1]]);
  }
};

#define ISINDEX(x) ((x) == NULL || PyInt_Check(x) || PyLong_Check(x) || PyIndex_Check(x))
static PyObject * apply_slice(PyObject *u, PyObject *v, PyObject *w) {
  PyTypeObject *tp = u->ob_type;
  PySequenceMethods *sq = tp->tp_as_sequence;

  if (sq && sq->sq_slice && ISINDEX(v) && ISINDEX(w)) {
    Py_ssize_t ilow = 0, ihigh = PY_SSIZE_T_MAX;
    if (!_PyEval_SliceIndex(v, &ilow)) return NULL;
    if (!_PyEval_SliceIndex(w, &ihigh)) return NULL;
    return PySequence_GetSlice(u, ilow, ihigh);
  } else {
    PyObject *slice = PySlice_New(v, w, NULL);
    if (slice != NULL) {
      PyObject *res = PyObject_GetItem(u, slice);
      Py_DECREF(slice);
      return res;
    } else return NULL;
  }
}

struct Slice: public RegOpImpl<RegOp<4>, Slice> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* state, RegOp<4> op, PyObject** registers) {
    PyObject* list = registers[op.reg[0]];
    PyObject* left = op.reg[1] != kInvalidRegister ? registers[op.reg[1]] : NULL;
    PyObject* right = op.reg[2] != kInvalidRegister ? registers[op.reg[2]] : NULL;
    registers[op.reg[3]] = apply_slice(list, left, right);
  }
};

template<int Opcode>
struct BadOp {
  static f_inline void eval(Evaluator *eval, RegisterFrame* state, PyObject** registers) {
    const char* name = OpUtil::name(Opcode);
    throw RException(PyExc_SystemError, StringPrintf("Bad opcode %s", name));
  }
};

#define CONCAT(...) __VA_ARGS__

#define REGISTER_OP(opname)\
    static int _force_register_ ## opname = LabelRegistry::add_label(opname, &&op_ ## opname);

#define _DEFINE_OP(opname, impl)\
      /*collectInfo(opname);\*/\
      impl::eval(this, frame, &pc, registers);\
      goto *labels[frame->next_code(pc)];

#define DEFINE_OP(opname, impl)\
    op_##opname:\
      _DEFINE_OP(opname, impl)

#define BAD_OP(opname)\
    op_##opname:\
     BadOp<opname>::eval(this, frame, registers);

#define FALLTHROUGH(opname) op_##opname:

#define INTEGER_OP(op)\
    a = PyInt_AS_LONG(v);\
    b = PyInt_AS_LONG(w);\
    i = (long)((unsigned long)a op b);\
    if ((i^a) < 0 && (i^b) < 0)\
        goto slow_path;\
    x = PyInt_FromLong(i);

#define BINARY_OP3(opname, objfn, intfn)\
    op_##opname: _DEFINE_OP(opname, BinaryOpWithSpecialization<CONCAT(opname, objfn, intfn)>)

#define BINARY_OP2(opname, objfn)\
    op_##opname: _DEFINE_OP(opname, BinaryOp<CONCAT(opname, objfn)>)

#define UNARY_OP2(opname, objfn)\
    op_##opname: _DEFINE_OP(opname, UnaryOp<CONCAT(opname, objfn)>)

PyObject * Evaluator::eval(RegisterFrame* frame) {
  Reg_Assert(frame != NULL, "NULL frame object.");
  Reg_Assert(PyTuple_GET_SIZE(frame->code->code()->co_cellvars) == 0, "Cell vars (closures) not supported.");

  register PyObject** registers = frame->registers;
  const char* pc = frame->instructions;
  PyObject* result = Py_None;

//  last_clock_ = rdtsc();

  static const void* const labels[] = {
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

  EVAL_LOG("New frame: %s", PyEval_GetFuncName(frame->code->function));
  try {
    goto *labels[frame->next_code(pc)];

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
    DEFINE_OP(BINARY_SUBSCR, BinarySubscr);

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

    UNARY_OP2(UNARY_INVERT, PyNumber_Invert);
    UNARY_OP2(UNARY_CONVERT, PyObject_Repr);
    UNARY_OP2(UNARY_NEGATIVE, PyNumber_Negative);
    UNARY_OP2(UNARY_POSITIVE, PyNumber_Positive);

    DEFINE_OP(UNARY_NOT, UnaryNot);

    DEFINE_OP(LOAD_FAST, LoadFast);
    DEFINE_OP(LOAD_LOCALS, LoadLocals);
    DEFINE_OP(LOAD_GLOBAL, LoadGlobal);
    DEFINE_OP(LOAD_NAME, LoadName);
    DEFINE_OP(LOAD_ATTR, LoadAttr);

    DEFINE_OP(STORE_NAME, StoreName);
    DEFINE_OP(STORE_ATTR, StoreAttr);
    DEFINE_OP(STORE_SUBSCR, StoreSubscr);
    DEFINE_OP(STORE_FAST, StoreFast);

    DEFINE_OP(CONST_INDEX, ConstIndex);

    DEFINE_OP(GET_ITER, GetIter);
    DEFINE_OP(FOR_ITER, ForIter);

    op_RETURN_VALUE: {
      result = ReturnValue::eval(this, frame, &pc, registers);
      goto done;
    }

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
    DEFINE_OP(COMPARE_OP, CompareOp);
    DEFINE_OP(INCREF, IncRef);
    DEFINE_OP(DECREF, DecRef);

    DEFINE_OP(LIST_APPEND, ListAppend);
    DEFINE_OP(SLICE, Slice);

    BAD_OP(SETUP_LOOP);
    BAD_OP(POP_BLOCK);
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
    BAD_OP(NOP);
    BAD_OP(ROT_FOUR);
    BAD_OP(DUP_TOP);
    BAD_OP(ROT_THREE);
    BAD_OP(ROT_TWO);
    BAD_OP(POP_TOP);
    BAD_OP(STOP_CODE);
    op_BADCODE: {
      EVAL_LOG("Jump to invalid opcode!?");
      throw RException(PyExc_SystemError, "Invalid jump.");
    }
  } catch (RException &error) {
    EVAL_LOG("Leaving frame: %s", PyEval_GetFuncName(frame->code->function));
    Reg_Assert(error.exception != NULL, "Error without exception set.");
    error.set_python_err();
    return NULL;
  }
  done:
  EVAL_LOG("Leaving frame: %s", PyEval_GetFuncName(frame->code->function));
  return result;
}
