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
  PyObject* obj;

  f_inline RefHelper(PyObject* o) :
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

// Wrapper around PyXXXObject*, allows implicit casting to PyObject.
template<class T>
struct PyObjHelper {
  T val_;
  PyObjHelper(T t) :
      val_(t) {
  }
  operator PyObject*() {
    return (PyObject*) val_;
  }
  operator T() {
    return val_;
  }
  operator void*() {
    return val_;
  }

  bool operator==(void* v) const {
    return val_ == v;
  }

  bool operator!=(void* v) const {
    return val_ != v;
  }

  T operator->() {
    return val_;
  }
};

RegisterFrame::RegisterFrame(RegisterCode* rcode, PyObject* obj, const ObjVector& args, const ObjVector& kw) :
    code(rcode) {
  instructions_ = code->instructions.data();
  current_hint = -1;

  if (rcode->function) {
    globals_ = PyFunction_GetGlobals(rcode->function);
    locals_ = NULL;
  } else {
    globals_ = PyEval_GetGlobals();
    locals_ = PyEval_GetGlobals();
  }

  Reg_Assert(kw.empty(), "Keyword args not supported.");

  builtins_ = PyEval_GetBuiltins();

  py_call_args = NULL;
  names_ = code->names();
  consts_ = code->consts();

  registers = new PyObject*[rcode->num_registers];

  const int num_freevars = PyTuple_GET_SIZE(rcode->code()->co_freevars);
  const int num_cellvars = PyTuple_GET_SIZE(rcode->code()->co_cellvars);
  const int num_cells = num_freevars + num_cellvars;

  const int num_args = args.size();

  if (num_cells == 0) {
    freevars = NULL;
  } else {
    freevars = new PyObject*[num_cells];

    for (int i = 0; i < num_cellvars; ++i) {
      bool found_argname = false;
      char *cellname = PyString_AS_STRING(PyTuple_GET_ITEM(rcode->code()->co_cellvars, i));
      for (int arg_idx = 0; arg_idx < num_args; ++arg_idx) {
        char* argname = PyString_AS_STRING(PyTuple_GET_ITEM(rcode->code()->co_varnames, arg_idx));
        if (strcmp(cellname, argname) == 0) {
          PyObject* arg_value = args[arg_idx];
          freevars[i] = PyCell_New(arg_value);
          found_argname = true;
          break;
        }
      }
      if (!found_argname) {
        freevars[i] = PyCell_New(NULL);
      }
    }

    PyObject* closure = ((PyFunctionObject*) rcode->function)->func_closure;
    if (closure) {
      for (int i = num_cellvars; i < num_cells; ++i) {
        freevars[i] = PyTuple_GET_ITEM(closure, i - num_cellvars) ;
        Py_INCREF(freevars[i]);
      }
    } else {
      for (int i = num_cellvars; i < num_cells; ++i) {
        freevars[i] = PyCell_New(NULL);
      }
    }
  }

//  for (int i = 0; i < num_cells; ++i) {
//    Log_Info("Cell: %d [%d] %p", i, freevars[i]->ob_refcnt, freevars[i]);
//  }

  const int num_registers = code->num_registers;

  // setup const and local register aliases.
  int num_consts = PyTuple_GET_SIZE(consts());

  for (int i = 0; i < num_consts; ++i) {
    registers[i] = PyTuple_GET_ITEM(consts(), i) ;
    Py_INCREF(registers[i]);
  }

  int needed_args = code->code()->co_argcount;
  int offset = num_consts;
  if (PyMethod_Check(obj)) {
    PyObject* self = PyMethod_GET_SELF(obj);

    Reg_Assert(self != NULL, "Method call without a bound self.");
    registers[offset++] = self;
    Py_INCREF(self);

    needed_args--;
  }

  if (code->function) {
    PyObject* def_args = PyFunction_GET_DEFAULTS(code->function);
    int num_def_args = def_args == NULL ? 0 : PyTuple_GET_SIZE(def_args);
    int num_args = args.size();
    if (num_args + num_def_args < needed_args) {
      throw RException(PyExc_TypeError, "Wrong number of arguments for %s, expected %d, got %d.",
                       PyEval_GetFuncName(code->function), needed_args - num_def_args, num_args);
    }

    for (int i = 0; i < needed_args; ++i) {
      PyObject* v = (i < num_args) ? args[i] : PyTuple_GET_ITEM(def_args, i - num_args) ;
      Py_INCREF(v);
      registers[offset++] = v;
    }
  }

  bzero(registers + offset, (num_registers - offset) * sizeof(PyObject*));
}

RegisterFrame::~RegisterFrame() {
  Py_XDECREF(py_call_args);

  const int num_registers = code->num_registers;
  register int i;
  for (i = 0; i < num_registers; ++i) {
    Py_XDECREF(registers[i]);
  }
  delete[] registers;

  const int num_freevars = PyTuple_GET_SIZE(code->code()->co_freevars);
  const int num_cellvars = PyTuple_GET_SIZE(code->code()->co_cellvars);
  const int num_cells = num_freevars + num_cellvars;
  for (i = 0; i < num_cells; ++i) {
//    Log_Info("Cell: %d [%d] %p", i, freevars[i]->ob_refcnt, freevars[i]);
    Py_XDECREF(freevars[i]);
  }
  delete[] freevars;
}

Evaluator::Evaluator() {
  bzero(op_counts_, sizeof(op_counts_));
  bzero(op_times_, sizeof(op_times_));
  total_count_ = 0;
  last_clock_ = 0;
  compiler_ = new Compiler;
  bzero(hints, sizeof(Hint) * kMaxHints);
}

Evaluator::~Evaluator() {
  delete compiler_;
}

PyObject* Evaluator::eval_python(PyObject* func, PyObject* args) {
  RegisterFrame* frame = frame_from_pyfunc(func, args, NULL);
  PyObject* result = eval(frame);
  delete frame;
  return result;
}

RegisterFrame* Evaluator::frame_from_pyframe(PyFrameObject* frame) {
  RegisterCode* regcode = compile((PyObject*) frame->f_code);

  ObjVector v_args;
  ObjVector kw_args;
  RegisterFrame* f = new RegisterFrame(regcode, (PyObject*) frame->f_code, v_args, kw_args);
  PyFrame_FastToLocals(frame);
  f->fill_locals(frame->f_locals);
  return f;
}

RegisterFrame* Evaluator::frame_from_pyfunc(PyObject* obj, PyObject* args, PyObject* kw) {
  if (args == NULL || !PyTuple_Check(args)) {
    throw RException(PyExc_TypeError, "Expected function argument tuple, got: %s", obj_to_str(PyObject_Type(args)));
  }

  RegisterCode* regcode = compile(obj);

  ObjVector v_args;
  for (int i = 0; i < PyTuple_GET_SIZE(args) ; ++i) {
    v_args.push_back(PyTuple_GET_ITEM(args, i) );
  }

  ObjVector kw_args;
  if (kw != NULL && kw != Py_None) {
    Log_Fatal("Keywords not supported.");
//    kw_args.push_back()
  }

  return new RegisterFrame(regcode, obj, v_args, kw_args);
}

RegisterFrame* Evaluator::frame_from_codeobj(PyObject* code) {
  ObjVector args, kw;
  RegisterCode *regcode = compile(code);
  return new RegisterFrame(regcode, code, args, kw);
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

#define SET_REGISTER(regnum, val)\
    Py_XDECREF(registers[regnum]);\
    CHECK_VALID(val);\
    registers[regnum] = val;

template<class OpType, class SubType>
struct RegOpImpl {
  static f_inline void eval(Evaluator* eval, RegisterFrame* frame, const char** pc, PyObject** registers) {
    OpType& op = *((OpType*) *pc);
    EVAL_LOG("%s -- %5d: %s", frame->str().c_str(), frame->offset(*pc), op.str(registers).c_str());
    *pc += op.size();
    SubType::_eval(eval, frame, op, registers);
  }
};

template<class SubType>
struct VarArgsOpImpl {
  static f_inline void eval(Evaluator* eval, RegisterFrame* frame, const char** pc, PyObject** registers) {
    VarRegOp *op = (VarRegOp*) (*pc);
    EVAL_LOG("%s -- %5d: %s", frame->str().c_str(), frame->offset(*pc), op->str(registers).c_str());
    *pc += op->size();
    SubType::_eval(eval, frame, op, registers);
  }
};

template<class SubType>
struct BranchOpImpl {
  static f_inline void eval(Evaluator* eval, RegisterFrame* frame, const char** pc, PyObject** registers) {
    BranchOp& op = *((BranchOp*) *pc);
    EVAL_LOG("%s -- %5d: %s", frame->str().c_str(), frame->offset(*pc), op.str().c_str());
    SubType::_eval(eval, frame, op, pc, registers);
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
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    PyObject* r2 = registers[op.reg[1]];
    PyObject* r3 = NULL;
    r3 = IntegerF(r1, r2);
    if (r3 == NULL) {
      r3 = ObjF(r1, r2);
    }
    SET_REGISTER(op.reg[2], r3);
  }
};

template<int OpCode, BinaryFunction ObjF>
struct BinaryOp: public RegOpImpl<RegOp<3>, BinaryOp<OpCode, ObjF> > {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    PyObject* r2 = registers[op.reg[1]];
    CHECK_VALID(r1);
    CHECK_VALID(r2);
    PyObject* r3 = ObjF(r1, r2);
    SET_REGISTER(op.reg[2], r3);
  }
};

template<int OpCode, UnaryFunction ObjF>
struct UnaryOp: public RegOpImpl<RegOp<2>, UnaryOp<OpCode, ObjF> > {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    CHECK_VALID(r1);
    PyObject* r2 = ObjF(r1);
    SET_REGISTER(op.reg[1], r2);
  }
};

struct UnaryNot: public RegOpImpl<RegOp<2>, UnaryNot> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    PyObject* res = PyObject_IsTrue(r1) ? Py_False : Py_True;
    Py_INCREF(res);
    SET_REGISTER(op.reg[1], res);
  }
};

struct BinaryModulo: public RegOpImpl<RegOp<3>, BinaryModulo> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    CHECK_VALID(r1);
    PyObject* r2 = registers[op.reg[1]];
    CHECK_VALID(r2);
    PyObject* r3 = IntegerOps::mod(r1, r2);
    if (r3 == NULL) {
      if (PyString_CheckExact(r1)) {
        r3 = PyString_Format(r1, r2);
      } else {
        r3 = PyNumber_Remainder(r1, r2);
      }
    }

    SET_REGISTER(op.reg[2], r3);
  }
};

struct BinaryPower: public RegOpImpl<RegOp<3>, BinaryPower> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    CHECK_VALID(r1);
    PyObject* r2 = registers[op.reg[1]];
    CHECK_VALID(r2);
    PyObject* r3 = PyNumber_Power(r1, r2, Py_None);
    CHECK_VALID(r3);

    SET_REGISTER(op.reg[2], r3);
  }
};

struct BinarySubscr: public RegOpImpl<RegOp<3>, BinarySubscr> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, PyObject** registers) {
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
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, PyObject** registers) {
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
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, PyObject** registers) {
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
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, PyObject** registers) {
    CHECK_VALID(registers[op.reg[0]]);
    Py_INCREF(registers[op.reg[0]]);
  }
};

struct DecRef: public RegOpImpl<RegOp<1>, DecRef> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, PyObject** registers) {
    CHECK_VALID(registers[op.reg[0]]);
    Py_DECREF(registers[op.reg[0]]);
  }
};

struct LoadLocals: public RegOpImpl<RegOp<1>, LoadLocals> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, PyObject** registers) {
    Py_XDECREF(registers[op.reg[0]]);
    Py_INCREF(frame->locals());
    registers[op.reg[0]] = frame->locals();
  }
};

struct LoadGlobal: public RegOpImpl<RegOp<1>, LoadGlobal> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, PyObject** registers) {
    PyObject* r1 = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyObject* r2 = PyDict_GetItem(frame->globals(), r1);
    if (r2 == NULL) {
      r2 = PyDict_GetItem(frame->builtins(), r1);
    }
    if (r2 == NULL) {
      throw RException(PyExc_NameError, "Global name %.200s not defined.", obj_to_str(r1));
    }
    Py_INCREF(r2);
    Py_XDECREF(registers[op.reg[0]]);
    CHECK_VALID(r2);
    registers[op.reg[0]] = r2;
  }
};

struct StoreGlobal: public RegOpImpl<RegOp<1>, StoreGlobal> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, PyObject** registers) {
    PyObject* key = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyObject* val = registers[op.reg[0]];
    PyDict_SetItem(frame->globals(), key, val);
  }
};

struct DeleteGlobal: public RegOpImpl<RegOp<0>, DeleteGlobal> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<0>& op, PyObject** registers) {
    PyObject* key = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyDict_DelItem(frame->globals(), key);
  }
};

struct LoadName: public RegOpImpl<RegOp<1>, LoadName> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, PyObject** registers) {
    PyObject* r1 = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyObject* r2 = PyDict_GetItem(frame->locals(), r1);
    if (r2 == NULL) {
      r2 = PyDict_GetItem(frame->globals(), r1);
    }
    if (r2 == NULL) {
      r2 = PyDict_GetItem(frame->builtins(), r1);
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

struct StoreName: public RegOpImpl<RegOp<1>, StoreName> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, PyObject** registers) {
    PyObject* r1 = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyObject* r2 = registers[op.reg[0]];
    CHECK_VALID(r1);
    CHECK_VALID(r2);
    PyObject_SetItem(frame->locals(), r1, r2);
  }
};

struct LoadFast: public RegOpImpl<RegOp<2>, LoadFast> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, PyObject** registers) {
    Py_INCREF(registers[op.reg[0]]);
    Py_XDECREF(registers[op.reg[1]]);
    CHECK_VALID(registers[op.reg[0]]);
    registers[op.reg[1]] = registers[op.reg[0]];
  }
};

struct StoreFast: public RegOpImpl<RegOp<2>, StoreFast> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, PyObject** registers) {
    Py_XDECREF(registers[op.reg[1]]);
    Py_INCREF(registers[op.reg[0]]);
    CHECK_VALID(registers[op.reg[0]]);
    registers[op.reg[1]] = registers[op.reg[0]];
  }
};

struct StoreAttr: public RegOpImpl<RegOp<2>, StoreAttr> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, PyObject** registers) {
    PyObject* obj = registers[op.reg[0]];
    PyObject* key = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyObject* value = registers[op.reg[1]];
    CHECK_VALID(obj);
    CHECK_VALID(key);
    CHECK_VALID(value);
    if (PyObject_SetAttr(obj, key, value) != 0) {
      throw RException();
    }
  }
};

struct StoreSubscr: public RegOpImpl<RegOp<3>, StoreSubscr> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, PyObject** registers) {
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
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, PyObject** registers) {
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

static PyDictObject* obj_getdictptr(PyObject* obj, PyTypeObject* type) {
  Py_ssize_t dictoffset;
  PyObject **dictptr;

  dictoffset = type->tp_dictoffset;
  if (dictoffset != 0) {
    if (dictoffset < 0) {
      Py_ssize_t tsize;
      size_t size;

      tsize = ((PyVarObject *) obj)->ob_size;
      if (tsize < 0) tsize = -tsize;
      size = _PyObject_VAR_SIZE(type, tsize);

      dictoffset += (long) size;
      assert(dictoffset > 0);
      assert(dictoffset % SIZEOF_VOID_P == 0);
    }
    dictptr = (PyObject **) ((char *) obj + dictoffset);
    return (PyDictObject*) *dictptr;
  }
  return NULL;
}

static size_t dict_getoffset(PyDictObject* dict, PyObject* key) {
  PyDictEntry* pos = dict->ma_table;
  for (int offset = 0; offset < dict->ma_mask + 1; ++offset, ++pos) {
    if (pos->me_key == key) {
      return offset;
    }
  }

  throw RException(PyExc_SystemError, "Requested offset of non-existent key!");
}

// LOAD_ATTR is common enough to warrant inlining some common code.
// Most of this is taken from _PyObject_GenericGetAttrWithDict
static PyObject * obj_getattr(Evaluator* eval, RegOp<2>& op, PyObject *obj, PyObject *name) {
  PyObjHelper<PyTypeObject*> type(Py_TYPE(obj) );
  PyObject *res = NULL;

//  static int op_hits = 0;
//  static int key_hits = 0;
//  static int count = 0;
//  ++count;
//  if (op.hint != kInvalidHint) {
//    ++op_hits;
//  }
//
//  EVERY_N(100000, Log_Info("%d/%d/%d", key_hits, op_hits, count));

  if (op.hint != kInvalidHint) {
    Hint h = eval->hints[op.hint];
    if (h.obj == type && h.key == name) {
      PyObjHelper<PyDictObject*> dict(obj_getdictptr(obj, type));
      PyDictEntry e(dict->ma_table[h.version]);
      if (e.me_key == name) {
//        ++key_hits;
        Py_INCREF(e.me_value);
        return e.me_value;
      }
    }
  }

  if (!PyString_Check(name)) {
    throw RException(PyExc_SystemError, "attribute name must be string, not '%.200s'", Py_TYPE(name) ->tp_name);
  }

  if (type->tp_dict == NULL) {
    if (PyType_Ready(type) < 0) {
      throw RException();
    }
  }

  PyObject* descr = _PyType_Lookup(type, name);
  descrgetfunc getter = NULL;
  if (descr != NULL && PyType_HasFeature(descr->ob_type, Py_TPFLAGS_HAVE_CLASS)) {
    getter = descr->ob_type->tp_descr_get;
    if (getter != NULL && PyDescr_IsData(descr)) {
      res = getter(descr, obj, (PyObject*) type);
      return res;
    }
  }

  // Look for a match in our object dictionary
  PyObjHelper<PyDictObject*> dict(obj_getdictptr(obj, type));
  if (dict != NULL) {
    res = PyDict_GetItem(dict, name);
  }

  // We found a match.  Create a hint for where to look in objects of this type.
  if (res != NULL) {
    size_t hint_pos = hint_offset(type, name);
    size_t dict_pos = dict_getoffset(dict, name);

    eval->hints[hint_pos] = {type, name, NULL, (unsigned int)dict_pos};
    op.hint = hint_pos;
    Py_INCREF(res);
    return res;
  }

  // Instance dictionary lookup failed, try to find a match in the class hierarchy.
  if (getter != NULL) {
    res = getter(descr, obj, (PyObject*) type);
  }

  if (res != NULL) {
    return res;
  }

  if (descr != NULL) {
    return descr;
  }

  throw RException(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", type->tp_name,
                   PyString_AS_STRING(name) );
}

struct LoadAttr: public RegOpImpl<RegOp<2>, LoadAttr> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, PyObject** registers) {
    PyObject* obj = registers[op.reg[0]];
    PyObject* name = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyObject* res = obj_getattr(eval, op, obj, name);
    SET_REGISTER(op.reg[1], res);
//    Py_INCREF(registers[op.reg[1]]);
  }
};

struct LoadDeref: public RegOpImpl<RegOp<1>, LoadDeref> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, PyObject** registers) {
    PyObject* closure_cell = frame->freevars[op.arg];
    PyObject* closure_value = PyCell_Get(closure_cell);
    SET_REGISTER(op.reg[0], closure_value);
  }
};

struct StoreDeref: public RegOpImpl<RegOp<1>, StoreDeref> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, PyObject** registers) {
    PyObject* value = registers[op.reg[0]];
    PyObject* dest_cell = frame->freevars[op.arg];
    PyCell_Set(dest_cell, value);
  }
};

struct LoadClosure: public RegOpImpl<RegOp<1>, LoadClosure> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, PyObject** registers) {
    PyObject* closure_cell = frame->freevars[op.arg];
    Py_INCREF(closure_cell);
    SET_REGISTER(op.reg[0], closure_cell);
  }
};

struct MakeFunction: public VarArgsOpImpl<MakeFunction> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, VarRegOp *op, PyObject** registers) {
    PyObject* code = registers[op->reg[0]];
    PyObject* func = PyFunction_New(code, frame->globals());
    PyObject* defaults = PyTuple_New(op->arg);
    for (int i = 0; i < op->arg; ++i) {
      PyTuple_SetItem(defaults, i, registers[op->reg[i + 1]]);
    }
    PyFunction_SetDefaults(func, defaults);
    SET_REGISTER(op->reg[op->arg + 1], func);
  }
};

struct MakeClosure: public VarArgsOpImpl<MakeClosure> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, VarRegOp *op, PyObject** registers) {
    // first register argument is the code object
    // second is the closure args tuple
    // rest of the registers are default argument values
    PyObject* code = registers[op->reg[0]];
    PyObject* func = PyFunction_New(code, frame->globals());
    PyObject* closure_values = registers[op->reg[1]];
    PyFunction_SetClosure(func, closure_values);

    PyObject* defaults = PyTuple_New(op->arg);
    for (int i = 0; i < op->arg; ++i) {
      PyTuple_SetItem(defaults, i, registers[op->reg[i + 2]]);
    }
    PyFunction_SetDefaults(func, defaults);
    SET_REGISTER(op->reg[op->arg + 2], func);
  }
};

struct CallFunction: public VarArgsOpImpl<CallFunction> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, VarRegOp *op, PyObject** registers) {
    int na = op->arg & 0xff;
    int nk = (op->arg >> 8) & 0xff;
    int n = nk * 2 + na;
    int i;
    PyObject* fn = registers[op->reg[n]];
    assert(n + 2 == op->num_registers);

    PyObject* res = NULL;
    RegisterCode* code = NULL;

    if (!PyCFunction_Check(fn)) {
      try {
        code = eval->compile(fn);
      } catch (RException& e) {
        Log_Info("Failed to compile function, executing using ceval: %s", obj_to_str(e.value));
        code = NULL;
      }
    }

    if (code == NULL || nk > 0) {
      if (frame->py_call_args == NULL || PyTuple_GET_SIZE(frame->py_call_args) != na) {
        Py_XDECREF(frame->py_call_args);
        frame->py_call_args = PyTuple_New(na);
      }

      PyObject* args = frame->py_call_args;
      for (i = 0; i < na; ++i) {
        CHECK_VALID(registers[op->reg[i]]);
        Py_INCREF(registers[op->reg[i]]);
        PyTuple_SET_ITEM(args, i, registers[op->reg[i]]);
      }

      PyObject* kwdict = NULL;
      if (nk > 0) {
        kwdict = PyDict_New();
        for (i = na; i < nk * 2; i += 2) {
          CHECK_VALID(registers[op->reg[i]]);
          CHECK_VALID(registers[op->reg[i+i]]);
          Py_INCREF(registers[op->reg[i]]);
          Py_INCREF(registers[op->reg[i+1]]);
          PyDict_SetItem(kwdict, registers[op->reg[i]], registers[op->reg[i + 1]]);
        }
      }
      if (PyCFunction_Check(fn)) {
        res = PyCFunction_Call(fn, args, kwdict);
      } else {
        res = PyObject_Call(fn, args, kwdict);
      }
    } else {
      ObjVector args, kw;
//      ObjVector& args = frame->reg_call_args;
//      args.resize(na);
      for (i = 0; i < na; ++i) {
        CHECK_VALID(registers[op->reg[i]]);
        args.push_back(registers[op->reg[i]]);
      }
      RegisterFrame f(code, fn, args, kw);
      res = eval->eval(&f);
    }

    if (res == NULL) {
      throw RException();
    }

    int dst = op->reg[n + 1];
    if (dst != kInvalidRegister) {
      SET_REGISTER(dst, res);
    }
  }
};

struct GetIter: public RegOpImpl<RegOp<2>, GetIter> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, PyObject** registers) {
    PyObject* res = PyObject_GetIter(registers[op.reg[0]]);
    Py_XDECREF(registers[op.reg[1]]);
    registers[op.reg[1]] = res;
  }
};

struct ForIter: public BranchOpImpl<ForIter> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *frame, BranchOp op, const char **pc,
                             PyObject** registers) {
    CHECK_VALID(registers[op.reg[0]]);
    PyObject* r1 = PyIter_Next(registers[op.reg[0]]);
    if (r1) {
      Py_XDECREF(registers[op.reg[1]]);
      registers[op.reg[1]] = r1;
      *pc += sizeof(BranchOp);
    } else {
      *pc = frame->instructions() + op.label;
    }

  }
};

struct JumpIfFalseOrPop: public BranchOpImpl<JumpIfFalseOrPop> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *frame, BranchOp op, const char **pc,
                             PyObject** registers) {
    PyObject *r1 = registers[op.reg[0]];
    if (r1 == Py_False || (PyObject_IsTrue(r1) == 0)) {
//      EVAL_LOG("Jumping: %s -> %d", obj_to_str(r1), op.label);
      *pc = frame->instructions() + op.label;
    } else {
      *pc += sizeof(BranchOp);
    }

  }
};

struct JumpIfTrueOrPop: public BranchOpImpl<JumpIfTrueOrPop> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *frame, BranchOp op, const char **pc,
                             PyObject** registers) {
    PyObject* r1 = registers[op.reg[0]];
    if (r1 == Py_True || (PyObject_IsTrue(r1) == 1)) {
      *pc = frame->instructions() + op.label;
    } else {
      *pc += sizeof(BranchOp);
    }

  }
};

struct JumpAbsolute: public BranchOpImpl<JumpAbsolute> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *frame, BranchOp op, const char **pc,
                             PyObject** registers) {
    EVAL_LOG("Jumping to: %d", op.label);
    *pc = frame->instructions() + op.label;
  }
};

struct BreakLoop: public BranchOpImpl<BreakLoop> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *frame, BranchOp op, const char **pc,
                             PyObject** registers) {
    EVAL_LOG("Jumping to: %d", op.label);
    *pc = frame->instructions() + op.label;
  }
};

// Evaluation of RETURN_VALUE is special.  g++ exceptions are excrutiatingly slow, so we
// can't use the exception mechanism to jump to our exit point.  Instead, we return a value
// here and jump to the exit of our frame.
struct ReturnValue {
  static f_inline PyObject* eval(Evaluator* eval, RegisterFrame* frame, const char** pc, PyObject** registers) {
    RegOp<1>& op = *((RegOp<1>*) *pc);
    EVAL_LOG("%5d: %s", frame->offset(*pc), op.str(registers).c_str());
    PyObject* result = registers[op.reg[0]];
    Py_INCREF(result);
    return result;
  }
};

struct Nop: public RegOpImpl<RegOp<0>, Nop> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<0>& op, PyObject** registers) {

  }
};

struct BuildTuple: public VarArgsOpImpl<BuildTuple> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, VarRegOp *op, PyObject** registers) {
    int i;
    PyObject* t = PyTuple_New(op->arg);
    for (i = 0; i < op->arg; ++i) {
      PyObject* v = registers[op->reg[i]];
      Py_INCREF(v);
      PyTuple_SET_ITEM(t, i, v);
    }
    SET_REGISTER(op->reg[op->arg], t);
  }
};

struct BuildList: public VarArgsOpImpl<BuildList> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, VarRegOp *op, PyObject** registers) {
    int i;
    PyObject* t = PyList_New(op->arg);
    for (i = 0; i < op->arg; ++i) {
      PyObject* v = registers[op->reg[i]];
      Py_INCREF(v);
      PyList_SET_ITEM(t, i, v);
    }
    SET_REGISTER(op->reg[op->arg], t);
  }
};

struct BuildClass: public RegOpImpl<RegOp<4>, BuildClass> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<4>& op, PyObject** registers) {
    PyObject* methods = registers[op.reg[0]];
    PyObject* bases = registers[op.reg[1]];
    PyObject* name = registers[op.reg[2]];

    // Begin: build_class from ceval.c
    PyObject *metaclass = NULL, *result, *base;

    if (PyDict_Check(methods)) metaclass = PyDict_GetItemString(methods, "__metaclass__");
    if (metaclass != NULL)
    Py_INCREF(metaclass);
    else if (PyTuple_Check(bases) && PyTuple_GET_SIZE(bases) > 0) {
      base = PyTuple_GET_ITEM(bases, 0) ;
      metaclass = PyObject_GetAttrString(base, "__class__");
      if (metaclass == NULL) {
        PyErr_Clear();
        metaclass = (PyObject *)base->ob_type;
        Py_INCREF(metaclass);
      }
    }
    else {
      PyObject *g = PyEval_GetGlobals();
      if (g != NULL && PyDict_Check(g))
      metaclass = PyDict_GetItemString(g, "__metaclass__");
      if (metaclass == NULL)
      metaclass = (PyObject *) &PyClass_Type;
      Py_INCREF(metaclass);
    }

    result = PyObject_CallFunctionObjArgs(metaclass, name, bases, methods, NULL);
    EVAL_LOG(
        "Building a class -- methods: %s\n bases: %s\n name: %s\n result: %s", obj_to_str(methods), obj_to_str(bases), obj_to_str(name), obj_to_str(result));

    Py_DECREF(metaclass);

    if (result == NULL && PyErr_ExceptionMatches(PyExc_TypeError)) {
      /* A type error here likely means that the user passed
       in a base that was not a class (such the random module
       instead of the random.random type).  Help them out with
       by augmenting the error message with more information.*/

      PyObject *ptype, *pvalue, *ptraceback;

      PyErr_Fetch(&ptype, &pvalue, &ptraceback);
      if (PyString_Check(pvalue)) {
        PyObject *newmsg;
        newmsg = PyString_FromFormat("Error when calling the metaclass bases\n"
                                     "    %s",
                                     PyString_AS_STRING(pvalue) );
        if (newmsg != NULL) {
          Py_DECREF(pvalue);
          pvalue = newmsg;
        }
      }

      PyErr_Restore(ptype, pvalue, ptraceback);
      throw RException();
    }

    // End: build_class()
    SET_REGISTER(op.reg[3], result);
  }
};

struct PrintItem: public RegOpImpl<RegOp<2>, PrintItem> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, PyObject** registers) {
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
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, PyObject** registers) {
    PyObject* w = op.reg[0] != kInvalidRegister ? registers[op.reg[0]] : PySys_GetObject((char*) "stdout");
    int err = PyFile_WriteString("\n", w);
    if (err == 0) PyFile_SoftSpace(w, 0);
  }
};

struct ListAppend: public RegOpImpl<RegOp<2>, ListAppend> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, PyObject** registers) {
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
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<4>& op, PyObject** registers) {
    PyObject* list = registers[op.reg[0]];
    PyObject* left = op.reg[1] != kInvalidRegister ? registers[op.reg[1]] : NULL;
    PyObject* right = op.reg[2] != kInvalidRegister ? registers[op.reg[2]] : NULL;
    registers[op.reg[3]] = apply_slice(list, left, right);
  }
};

template<int Opcode>
struct BadOp {
  static n_inline void eval(Evaluator *eval, RegisterFrame* frame, PyObject** registers) {
    const char* name = OpUtil::name(Opcode);
    throw RException(PyExc_SystemError, "Bad opcode %s", name);
  }
};

// Imports

struct ImportName: public RegOpImpl<RegOp<3>, ImportName> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, RegOp<3>& op, PyObject** registers) {
    PyObject* name = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyObject* import = PyDict_GetItemString(frame->builtins(), "__import__");
    if (import == NULL) {
      throw RException(PyExc_ImportError, "__import__ not found in builtins.");
    }

    PyObject* args = NULL;
    PyObject* v = registers[op.reg[0]];
    PyObject* u = registers[op.reg[1]];
    if (PyInt_AsLong(u) != -1 || PyErr_Occurred()) {
      PyErr_Clear();
      args = PyTuple_Pack(5, name, frame->globals(), frame->locals(), v, u);
    } else {
      args = PyTuple_Pack(4, name, frame->globals(), frame->locals(), v);
    }

    PyObject* res = PyEval_CallObject(import, args);
    Py_XDECREF(registers[op.reg[2]]);
    registers[op.reg[2]] = res;
    CHECK_VALID(registers[op.reg[2]]);
  }
};

struct ImportStar: public RegOpImpl<RegOp<1>, ImportStar> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, RegOp<1>& op, PyObject** registers) {
    PyObject* module = registers[op.reg[0]];
    PyObject *all = PyObject_GetAttrString(module, "__all__");
    bool skip_leading_underscores = (all == NULL);
    if (all == NULL) {
      PyObject* dict = PyObject_GetAttrString(module, "__dict__");
      all = PyMapping_Keys(dict);
    }

    for (int pos = 0, err = 0;; pos++) {
      PyObject* name = PySequence_GetItem(all, pos);
      if (name == NULL) {
        if (!PyErr_ExceptionMatches(PyExc_IndexError)) err = -1;
        else PyErr_Clear();
        break;
      }
      if (skip_leading_underscores && PyString_Check(name) && PyString_AS_STRING(name) [0] == '_') {
        Py_DECREF(name);
        continue;
      }

      PyObject* value = PyObject_GetAttr(module, name);
      if (value == NULL) err = -1;
      else {
        PyObject_SetItem(frame->locals(), name, value);
      }
      Py_DECREF(name);
      Py_XDECREF(value);
      if (err != 0) break;
    }
  }
};

struct ImportFrom: public RegOpImpl<RegOp<2>, ImportFrom> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, RegOp<2>& op, PyObject** registers) {
    PyObject* name = PyTuple_GetItem(frame->names(), op.arg);
    PyObject* module = registers[op.reg[0]];
    Py_XDECREF(registers[op.reg[1]]);
    PyObject* val = PyObject_GetAttr(module, name);
    if (val == NULL) {
      if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        throw RException(PyExc_ImportError, "cannot import name %.230s", PyString_AsString(name));
      } else {
        throw RException();
      }
    }

    registers[op.reg[1]] = val;
  }
};

#define CONCAT(...) __VA_ARGS__

#define REGISTER_OP(opname)\
    static int _force_register_ ## opname = LabelRegistry::add_label(opname, &&op_ ## opname);

#define JUMP_TO(opname)\
    goto *labels[opname]

#define _DEFINE_OP(opname, impl)\
      /*collectInfo(opname);\*/\
      impl::eval(this, frame, &pc, registers);\
      JUMP_TO(frame->next_code(pc));

#define DEFINE_OP(opname, impl)\
    op_##opname:\
      _DEFINE_OP(opname, impl)

#define BAD_OP(opname)\
    op_##opname:\
     BadOp<opname>::eval(this, frame, registers);

#define FALLTHROUGH(opname) op_##opname:

#define BINARY_OP3(opname, objfn, intfn)\
    op_##opname: _DEFINE_OP(opname, BinaryOpWithSpecialization<CONCAT(opname, objfn, intfn)>)

#define BINARY_OP2(opname, objfn)\
    op_##opname: _DEFINE_OP(opname, BinaryOp<CONCAT(opname, objfn)>)

#define UNARY_OP2(opname, objfn)\
    op_##opname: _DEFINE_OP(opname, UnaryOp<CONCAT(opname, objfn)>)

PyObject * Evaluator::eval(RegisterFrame* f) {
  register RegisterFrame* frame = f;
  register PyObject** registers = frame->registers;
  register const char* pc = frame->instructions();

  Reg_Assert(frame != NULL, "NULL frame object.");
  // Reg_Assert(PyTuple_GET_SIZE(frame->code->code()->co_cellvars) == 0, "Cell vars (closures) not supported.");

  PyObject* result = Py_None;

//  last_clock_ = rdtsc();

// #define OFFSET(opname) ((int64_t)&&op_##opname) - ((int64_t)&&op_STOP_CODE)
#define OFFSET(opname) &&op_##opname

static const void* labels[] = {
  OFFSET(STOP_CODE),
  OFFSET(POP_TOP),
  OFFSET(ROT_TWO),
  OFFSET(ROT_THREE),
  OFFSET(DUP_TOP),
  OFFSET(ROT_FOUR),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(NOP),
  OFFSET(UNARY_POSITIVE),
  OFFSET(UNARY_NEGATIVE),
  OFFSET(UNARY_NOT),
  OFFSET(UNARY_CONVERT),
  OFFSET(BADCODE),
  OFFSET(UNARY_INVERT),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(BINARY_POWER),
  OFFSET(BINARY_MULTIPLY),
  OFFSET(BINARY_DIVIDE),
  OFFSET(BINARY_MODULO),
  OFFSET(BINARY_ADD),
  OFFSET(BINARY_SUBTRACT),
  OFFSET(BINARY_SUBSCR),
  OFFSET(BINARY_FLOOR_DIVIDE),
  OFFSET(BINARY_TRUE_DIVIDE),
  OFFSET(INPLACE_FLOOR_DIVIDE),
  OFFSET(INPLACE_TRUE_DIVIDE),
  OFFSET(SLICE),
  OFFSET(SLICE),
  OFFSET(SLICE),
  OFFSET(SLICE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(STORE_SLICE),
  OFFSET(STORE_SLICE),
  OFFSET(STORE_SLICE),
  OFFSET(STORE_SLICE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(DELETE_SLICE),
  OFFSET(DELETE_SLICE),
  OFFSET(DELETE_SLICE),
  OFFSET(DELETE_SLICE),
  OFFSET(STORE_MAP),
  OFFSET(INPLACE_ADD),
  OFFSET(INPLACE_SUBTRACT),
  OFFSET(INPLACE_MULTIPLY),
  OFFSET(INPLACE_DIVIDE),
  OFFSET(INPLACE_MODULO),
  OFFSET(STORE_SUBSCR),
  OFFSET(DELETE_SUBSCR),
  OFFSET(BINARY_LSHIFT),
  OFFSET(BINARY_RSHIFT),
  OFFSET(BINARY_AND),
  OFFSET(BINARY_XOR),
  OFFSET(BINARY_OR),
  OFFSET(INPLACE_POWER),
  OFFSET(GET_ITER),
  OFFSET(BADCODE),
  OFFSET(PRINT_EXPR),
  OFFSET(PRINT_ITEM),
  OFFSET(PRINT_NEWLINE),
  OFFSET(PRINT_ITEM_TO),
  OFFSET(PRINT_NEWLINE_TO),
  OFFSET(INPLACE_LSHIFT),
  OFFSET(INPLACE_RSHIFT),
  OFFSET(INPLACE_AND),
  OFFSET(INPLACE_XOR),
  OFFSET(INPLACE_OR),
  OFFSET(BREAK_LOOP),
  OFFSET(WITH_CLEANUP),
  OFFSET(LOAD_LOCALS),
  OFFSET(RETURN_VALUE),
  OFFSET(IMPORT_STAR),
  OFFSET(EXEC_STMT),
  OFFSET(YIELD_VALUE),
  OFFSET(POP_BLOCK),
  OFFSET(END_FINALLY),
  OFFSET(BUILD_CLASS),
  OFFSET(STORE_NAME),
  OFFSET(DELETE_NAME),
  OFFSET(UNPACK_SEQUENCE),
  OFFSET(FOR_ITER),
  OFFSET(LIST_APPEND),
  OFFSET(STORE_ATTR),
  OFFSET(DELETE_ATTR),
  OFFSET(STORE_GLOBAL),
  OFFSET(DELETE_GLOBAL),
  OFFSET(DUP_TOPX),
  OFFSET(LOAD_CONST),
  OFFSET(LOAD_NAME),
  OFFSET(BUILD_TUPLE),
  OFFSET(BUILD_LIST),
  OFFSET(BUILD_SET),
  OFFSET(BUILD_MAP),
  OFFSET(LOAD_ATTR),
  OFFSET(COMPARE_OP),
  OFFSET(IMPORT_NAME),
  OFFSET(IMPORT_FROM),
  OFFSET(JUMP_FORWARD),
  OFFSET(JUMP_IF_FALSE_OR_POP),
  OFFSET(JUMP_IF_TRUE_OR_POP),
  OFFSET(JUMP_ABSOLUTE),
  OFFSET(POP_JUMP_IF_FALSE),
  OFFSET(POP_JUMP_IF_TRUE),
  OFFSET(LOAD_GLOBAL),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(CONTINUE_LOOP),
  OFFSET(SETUP_LOOP),
  OFFSET(SETUP_EXCEPT),
  OFFSET(SETUP_FINALLY),
  OFFSET(BADCODE),
  OFFSET(LOAD_FAST),
  OFFSET(STORE_FAST),
  OFFSET(DELETE_FAST),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(RAISE_VARARGS),
  OFFSET(CALL_FUNCTION),
  OFFSET(MAKE_FUNCTION),
  OFFSET(BUILD_SLICE),
  OFFSET(MAKE_CLOSURE),
  OFFSET(LOAD_CLOSURE),
  OFFSET(LOAD_DEREF),
  OFFSET(STORE_DEREF),
  OFFSET(BADCODE),
  OFFSET(BADCODE),
  OFFSET(CALL_FUNCTION_VAR),
  OFFSET(CALL_FUNCTION_KW),
  OFFSET(CALL_FUNCTION_VAR_KW),
  OFFSET(SETUP_WITH),
  OFFSET(BADCODE),
  OFFSET(EXTENDED_ARG),
  OFFSET(SET_ADD),
  OFFSET(MAP_ADD),
  OFFSET(INCREF),
  OFFSET(DECREF),
  OFFSET(CONST_INDEX)}
;

EVAL_LOG("Entering frame: %s", frame->str().c_str());
try {
    JUMP_TO(frame->next_code(pc));

BAD_OP(STOP_CODE);

BINARY_OP3(BINARY_MULTIPLY, PyNumber_Multiply, IntegerOps::mul);
BINARY_OP3(BINARY_DIVIDE, PyNumber_Divide, IntegerOps::div);
BINARY_OP3(BINARY_ADD, PyNumber_Add, IntegerOps::add);
BINARY_OP3(BINARY_SUBTRACT, PyNumber_Subtract, IntegerOps::sub);

BINARY_OP2(BINARY_OR, PyNumber_Or);
BINARY_OP2(BINARY_XOR, PyNumber_Xor);
BINARY_OP2(BINARY_AND, PyNumber_And);
BINARY_OP2(BINARY_RSHIFT, PyNumber_Rshift);
BINARY_OP2(BINARY_LSHIFT, PyNumber_Lshift);
BINARY_OP2(BINARY_TRUE_DIVIDE, PyNumber_TrueDivide);
BINARY_OP2(BINARY_FLOOR_DIVIDE, PyNumber_FloorDivide);

DEFINE_OP(BINARY_POWER, BinaryPower);
DEFINE_OP(BINARY_SUBSCR, BinarySubscr);
DEFINE_OP(BINARY_MODULO, BinaryModulo);

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
DEFINE_OP(LOAD_NAME, LoadName);
DEFINE_OP(LOAD_ATTR, LoadAttr);

DEFINE_OP(STORE_NAME, StoreName);
DEFINE_OP(STORE_ATTR, StoreAttr);
DEFINE_OP(STORE_SUBSCR, StoreSubscr);
DEFINE_OP(STORE_FAST, StoreFast);

DEFINE_OP(LOAD_GLOBAL, LoadGlobal);
DEFINE_OP(STORE_GLOBAL, StoreGlobal);
DEFINE_OP(DELETE_GLOBAL, DeleteGlobal);

DEFINE_OP(LOAD_CLOSURE, LoadClosure);
DEFINE_OP(LOAD_DEREF, LoadDeref);
DEFINE_OP(STORE_DEREF, StoreDeref);

DEFINE_OP(CONST_INDEX, ConstIndex);

DEFINE_OP(GET_ITER, GetIter);
DEFINE_OP(FOR_ITER, ForIter);
DEFINE_OP(BREAK_LOOP, BreakLoop);

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

DEFINE_OP(IMPORT_STAR, ImportStar);
DEFINE_OP(IMPORT_FROM, ImportFrom);
DEFINE_OP(IMPORT_NAME, ImportName);

DEFINE_OP(MAKE_FUNCTION, MakeFunction);
DEFINE_OP(MAKE_CLOSURE, MakeClosure);
DEFINE_OP(BUILD_CLASS, BuildClass);

BAD_OP(SETUP_LOOP);
BAD_OP(POP_BLOCK);
BAD_OP(LOAD_CONST);
BAD_OP(JUMP_FORWARD);
BAD_OP(MAP_ADD);
BAD_OP(SET_ADD);
BAD_OP(EXTENDED_ARG);
BAD_OP(SETUP_WITH);
BAD_OP(BUILD_SLICE);
BAD_OP(RAISE_VARARGS);
BAD_OP(DELETE_FAST);
BAD_OP(SETUP_FINALLY);
BAD_OP(SETUP_EXCEPT);
BAD_OP(CONTINUE_LOOP);
BAD_OP(BUILD_MAP);
BAD_OP(BUILD_SET);
BAD_OP(DUP_TOPX);
BAD_OP(DELETE_ATTR);
BAD_OP(UNPACK_SEQUENCE);
BAD_OP(DELETE_NAME);
BAD_OP(END_FINALLY);
BAD_OP(YIELD_VALUE);
BAD_OP(EXEC_STMT);
BAD_OP(WITH_CLEANUP);
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

op_BADCODE: {
      EVAL_LOG("Jump to invalid opcode!?");
throw RException(PyExc_SystemError, "Invalid jump.");
    }
  } catch (RException &error) {
    EVAL_LOG("ERROR: Leaving frame: %s", frame->str().c_str());
Reg_Assert(error.exception != NULL, "Error without exception set.");
error.set_python_err();
    return NULL;
  }
  done:
  EVAL_LOG("SUCCESS: Leaving frame: %s", frame->str().c_str());
return result;
}

//void StartTracing(Evaluator* eval) {
//  PyEval_SetProfile(&TraceFunction, (PyObject*)eval);
//}

// TODO(power) -- handle tracing method calls?
// Give up on this route?
//int TraceFunction(PyObject *obj, PyFrameObject *frame, int what, PyObject *arg) {
//  if (what != PyTrace_CALL) {
//    return 0;
//  }
//
//  Log_Info("Tracing... %s:%d %d (%p)", obj_to_str(frame->f_code->co_filename), frame->f_code->co_firstlineno, what, arg);
//  Evaluator* eval = (Evaluator*) obj;
//  try {
//    RegisterFrame* rframe = eval->frame_from_pyframe(frame);
//    PyCodeObject* code = PyCode_NewEmpty("*dummy*", "__dummy__", frame->f_lineno);
//    code->co_consts = PyTuple_Pack(1, rframe);
//  } catch (RException& e) {
//    Log_Info("Failed to compile %s:%d %d (%p), using python.", obj_to_str(frame->f_code->co_filename), frame->f_code->co_firstlineno, what, arg);
//  }
//
//  frame->f_code->co_code =
//
//  return 0;
//}
