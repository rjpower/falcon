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

// Register load/store helpers
#define STORE_REG(regnum, val)\
    decltype(val) v__ = val;\
    Register& r__ = registers[regnum];\
    r__.decref();\
    r__.store(v__);

#define LOAD_OBJ(regnum) registers[regnum].as_obj()
#define LOAD_INT(regnum) registers[regnum].as_int()
#define LOAD_FLOAT(regnum) registers[regnum].as_float()

typedef long (*IntegerBinaryOp)(long, long);
typedef PyObject* (*PythonBinaryOp)(PyObject*, PyObject*);
typedef PyObject* (*UnaryFunction)(PyObject*);

// Some scoped object helpers for tracking refcounts.
struct GilHelper {
  PyGILState_STATE state_;

  GilHelper() :
      state_(PyGILState_Ensure()) {
  }
  ~GilHelper() {
    PyGILState_Release(state_);
  }
};

// Catch bad case of RefHelper(x) (goes immediately out of scope
#define GilHelper(v) static int MustInitWithVar[-1];
#define RefHelper(v) static int MustInitWithVar[-1];

// Wrapper around PyXXXObject* which allows implicit casting to PyObject.
// This let's us access member variables and call API functions easily.
template<class T>
struct PyObjHelper {
  const T& val_;
  PyObjHelper(const T& t) :
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


  if (rcode->function) {
    globals_ = PyFunction_GetGlobals(rcode->function);
    locals_ = NULL;
  } else {
    globals_ = PyEval_GetGlobals();
    locals_ = PyEval_GetGlobals();
  }

  Reg_Assert(kw.empty(), "Keyword args not supported.");

  builtins_ = PyEval_GetBuiltins();

  names_ = code->names();
  consts_ = code->consts();

#if ! STACK_ALLOC_REGISTERS
  registers = new Register[rcode->num_registers];
#endif

  const int num_args = args.size();
  if (rcode->num_cells > 0) {
#if ! STACK_ALLOC_REGISTERS
    freevars = new PyObject*[rcode->num_cells];
#endif
    int i;
    for (i = 0; i < rcode->num_cellvars; ++i) {
      bool found_argname = false;
      char *cellname = PyString_AS_STRING(PyTuple_GET_ITEM(rcode->code()->co_cellvars, i));
      for (int arg_idx = 0; arg_idx < num_args; ++arg_idx) {
        char* argname = PyString_AS_STRING(PyTuple_GET_ITEM(rcode->code()->co_varnames, arg_idx));
        if (strcmp(cellname, argname) == 0) {
          PyObject* arg_value = args[arg_idx].as_obj();
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
      for (int i = rcode->num_cellvars; i < rcode->num_cells; ++i) {
        freevars[i] = PyTuple_GET_ITEM(closure, i - rcode->num_cellvars) ;
        Py_INCREF(freevars[i]);
      }
    } else {
      for (int i = rcode->num_cellvars; i < rcode->num_cells; ++i) {
        freevars[i] = PyCell_New(NULL);
      }
    }
  } else {
#if ! STACK_ALLOC_REGISTERS
            freevars = NULL;
#endif
          }

//  Log_Info("Alignments: reg: %d code: %d consts: %d globals: %d, this: %d",
//           ((long)registers) % 64, ((long)rcode->instructions.data()) % 64, (long)consts_ % 64, (long)globals_ % 64, (long)this % 64);

  const int num_registers = code->num_registers;

  // setup const and local register aliases.
  int num_consts = PyTuple_GET_SIZE(consts());
  for (int i = 0; i < num_consts; ++i) {
    PyObject* v = PyTuple_GET_ITEM(consts(), i) ;
    Py_INCREF(v);
    registers[i].store(v);
  }

  int needed_args = code->code()->co_argcount;
  int offset = num_consts;
  if (PyMethod_Check(obj)) {
    PyObject* self = PyMethod_GET_SELF(obj);

    Reg_Assert(self != NULL, "Method call without a bound self.");
    Py_INCREF(self);
    registers[offset].store(self);
    ++offset;

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
      if (i < num_args) {
        registers[offset].store(args[i]);
      } else {
        registers[offset].store(PyTuple_GET_ITEM(def_args, i - num_args) );
      }

      registers[offset].incref();
      ++offset;
    }
  }

  Reg_AssertLt(num_registers, kMaxRegisters);
  for (register int i = offset; i < num_registers; ++i) {
    registers[i].reset();
  }

}

RegisterFrame::~RegisterFrame() {
  const int num_registers = code->num_registers;
  for (register int i = 0; i < num_registers; ++i) {
    registers[i].decref();
  }

  for (register int i = 0; i < this->code->num_cells; ++i) {
    Py_XDECREF(freevars[i]);
  }

#if ! STACK_ALLOC_REGISTERS
  delete[] registers;
  delete[] freevars;
#endif
}

Evaluator::Evaluator() {
  bzero(op_counts_, sizeof(op_counts_));
  bzero(op_times_, sizeof(op_times_));
  total_count_ = 0;
  last_clock_ = 0;
  hint_hits_ = 0;
  hint_misses_ = 0;
  compiler_ = new Compiler;
  bzero(hints, sizeof(Hint) * kMaxHints);

  // We use a sentinel value for the invalid hint index.
  hints[kInvalidHint].guard.obj = NULL;
  hints[kInvalidHint].key = NULL;
  hints[kInvalidHint].value = NULL;
  hints[kInvalidHint].version = (unsigned int) -1;
}

Evaluator::~Evaluator() {
  delete compiler_;
}

void RegisterFrame::fill_locals(PyObject* ldict) {
  PyObject* varnames = code->varnames();
  for (int i = 0; i < PyTuple_GET_SIZE(varnames) ; ++i) {
    PyObject* name = PyTuple_GET_ITEM(varnames, i) ;
    PyObject* value = PyDict_GetItem(ldict, name);
    registers[num_consts() + i].store(value);
  }
  Py_INCREF(ldict);
  locals_ = ldict;
}

PyObject* RegisterFrame::locals() {
  if (!locals_) {
    locals_ = PyDict_New();
  }
  PyObject* varnames = code->varnames();
  const int num_consts = PyTuple_Size(consts());
  const int num_locals = code->code()->co_nlocals;
  for (int i = 0; i < num_locals; ++i) {
    PyObject* v = LOAD_OBJ(num_consts + i);
    if (v != NULL) {
      Py_INCREF(v);
      PyDict_SetItem(locals_, PyTuple_GetItem(varnames, i), v);
    }
  }
  return locals_;
}


//Register

PyObject* Evaluator::eval_python(PyObject* func, PyObject* args, PyObject* kw) {
  RegisterFrame* frame;
  try {
    frame = frame_from_pyfunc(func, args, kw);
  } catch (RException& r) {
    EVAL_LOG("Couldn't compile function, calling CPython...");
    return PyObject_Call(func, args, kw);
  }
  Register result = eval(frame);
  delete frame;
  return result.as_obj();
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
  v_args.resize(PyTuple_GET_SIZE(args) );
  for (size_t i = 0; i < v_args.size(); ++i) {
    v_args[i].store(PyTuple_GET_ITEM(args, i) );
  }

  ObjVector kw_args;

  size_t n_kwds = 0;
  if (kw != NULL && PyDict_Check(kw)) {
    n_kwds =  PyDict_Size(kw);
  }


  for (size_t i = 0; i < n_kwds; ++i) {
    throw RException(PyExc_ValueError, "Keywords not yet supported, n_given = %d", n_kwds);
    // should check whether kw is present in the args
    // and use default otherwise
    // kw_args.push_back()
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

template<class OpType, class SubType>
struct RegOpImpl {
  static f_inline const char* eval(Evaluator* eval, RegisterFrame* frame, const char* pc, Register* registers) {
    OpType& op = *((OpType*) pc);
    EVAL_LOG("%s -- %5d: %s", frame->str().c_str(), frame->offset(pc), op.str(registers).c_str());
    pc += op.size();
    SubType::_eval(eval, frame, op, registers);
    return pc;
  }
};

template<class SubType>
struct VarArgsOpImpl {
  static f_inline const char* eval(Evaluator* eval, RegisterFrame* frame, const char* pc, Register* registers) {
    VarRegOp *op = (VarRegOp*) pc;
    EVAL_LOG("%s -- %5d: %s", frame->str().c_str(), frame->offset(pc), op->str(registers).c_str());
    pc += op->size();
    SubType::_eval(eval, frame, op, registers);
    return pc;
  }
};

template<class OpType, class SubType>
struct BranchOpImpl {
  static f_inline const char* eval(Evaluator* eval, RegisterFrame* frame, const char* pc, Register* registers) {
    OpType& op = *((OpType*) pc);
    EVAL_LOG("%s -- %5d: %s", frame->str().c_str(), frame->offset(pc), op.str(registers).c_str());
    SubType::_eval(eval, frame, op, &pc, registers);
    return pc;
  }
};

#define OP_OVERFLOWED(a, b, i) ((i ^ a) < 0 && (i ^ b) < 0)

struct IntegerOps {
#define _OP(name, op)\
  static f_inline long name(long a, long b) {\
    return (long) ((unsigned long) a op b);\
  }

  _OP(add, +)
  _OP(sub, -)
  _OP(mul, *)
  _OP(div, /)
  _OP(mod, %)
  _OP(Or, |)
  _OP(Xor, ^)
  _OP(And, &)
  _OP(Rshift, >>)
  _OP(Lshift, <<)

  static f_inline PyObject* compare(long a, long b, int arg) {
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
      return a == b ? Py_True : Py_False ;
    case PyCmp_IS_NOT:
      return a != b ? Py_True : Py_False ;
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

    double a = PyFloat_AS_DOUBLE(w);
    double b = PyFloat_AS_DOUBLE(v);

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

template<int OpCode, PythonBinaryOp ObjF, IntegerBinaryOp IntegerF, bool CanOverFlow>
struct BinaryOpWithSpecialization: public RegOpImpl<RegOp<3>,
    BinaryOpWithSpecialization<OpCode, ObjF, IntegerF, CanOverFlow> > {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    Register& r1 = registers[op.reg[0]];
    Register& r2 = registers[op.reg[1]];

    if (r1.get_type() == IntType && r2.get_type() == IntType) {
      register long a = r1.as_int();
      register long b = r2.as_int();
      register long val = IntegerF(a, b);
      if (!CanOverFlow || !OP_OVERFLOWED(a, b, val)) {
        STORE_REG(op.reg[2], val);
        return;
      }
    }

    STORE_REG(op.reg[2], ObjF(r1.as_obj(), r2.as_obj()));
  }
};

template<int OpCode, PythonBinaryOp ObjF>
struct BinaryOp: public RegOpImpl<RegOp<3>, BinaryOp<OpCode, ObjF> > {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    PyObject* r1 = LOAD_OBJ(op.reg[0]);
    PyObject* r2 = LOAD_OBJ(op.reg[1]);
    CHECK_VALID(r1);
    CHECK_VALID(r2);
    PyObject* r3 = ObjF(r1, r2);
    STORE_REG(op.reg[2], r3);
  }
};




template<int OpCode, UnaryFunction ObjF>
struct UnaryOp: public RegOpImpl<RegOp<2>, UnaryOp<OpCode, ObjF> > {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, Register* registers) {
    PyObject* r1 = LOAD_OBJ(op.reg[0]);
    CHECK_VALID(r1);
    PyObject* r2 = ObjF(r1);
    STORE_REG(op.reg[1], r2);
  }
};

struct UnaryNot: public RegOpImpl<RegOp<2>, UnaryNot> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, Register* registers) {
    PyObject* r1 = LOAD_OBJ(op.reg[0]);
    PyObject* res = PyObject_IsTrue(r1) ? Py_False : Py_True;
    Py_INCREF(res);
    STORE_REG(op.reg[1], res);
  }
};

struct BinaryModulo: public RegOpImpl<RegOp<3>, BinaryModulo> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    Register& r1 = registers[op.reg[0]];
    Register& r2 = registers[op.reg[1]];

    if (r1.get_type() == IntType && r2.get_type() == IntType) {
      long x = r1.as_int();
      long y = r2.as_int();
      // C's modulo differs from Python's remainder when
      // args can be negative
      if (x >= 0 && y >= 0) {
        Register& dst = registers[op.reg[2]];
        dst.decref();
        dst.store(x % y);
        return;
      }
    }

    PyObject* o1 = r1.as_obj();
    PyObject* o2 = r2.as_obj();
    PyObject* dst = NULL;
    if (PyString_CheckExact(o1)) {
      dst = PyString_Format(o1, o2);
    } else {
      dst = PyNumber_Remainder(o1, o2);
    }
    if (!dst) {
      throw RException();
    }
    STORE_REG(op.reg[2], dst);
  }
};

struct BinaryPower: public RegOpImpl<RegOp<3>, BinaryPower> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    PyObject* r1 = LOAD_OBJ(op.reg[0]);
    CHECK_VALID(r1);
    PyObject* r2 = LOAD_OBJ(op.reg[1]);
    CHECK_VALID(r2);
    PyObject* r3 = PyNumber_Power(r1, r2, Py_None);
    CHECK_VALID(r3);

    STORE_REG(op.reg[2], r3);
  }
};


struct BinarySubscr: public RegOpImpl<RegOp<3>, BinarySubscr> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    PyObject* list = LOAD_OBJ(op.reg[0]);
    Register& key = registers[op.reg[1]];
    CHECK_VALID(list);
    PyObject* res = NULL;
    if (PyList_CheckExact(list) && key.get_type() == IntType) {
      Py_ssize_t i = key.as_int();
      if (i < 0) i += PyList_GET_SIZE(list);
      if (i >= 0 && i < PyList_GET_SIZE(list) ) {
        res = PyList_GET_ITEM(list, i);
        Py_INCREF(res);
        CHECK_VALID(res);
        STORE_REG(op.reg[2], res);
        return;
      }
    }

    res = PyObject_GetItem(list, key.as_obj());

    if (!res) {
      throw RException();
    }

    CHECK_VALID(res);
    STORE_REG(op.reg[2], res);
  }
};


struct BinarySubscrList : public RegOpImpl<RegOp<3>, BinarySubscrList> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    PyObject* list = LOAD_OBJ(op.reg[0]);
    Register& key = registers[op.reg[1]];
    CHECK_VALID(list);
    PyObject* res = NULL;
    if (key.get_type() == IntType) {
      Py_ssize_t i = key.as_int();
      Py_ssize_t n = PyList_GET_SIZE(list);
      if (i < 0) i += n;
      if (i >= 0 && i < n) {
        res = PyList_GET_ITEM(list, i);
        Py_INCREF(res);
        CHECK_VALID(res);
        STORE_REG(op.reg[2], res);
        return;
      }
    }
    res = PyObject_GetItem(list, key.as_obj());
    if (!res) {
      throw RException();
    }
    CHECK_VALID(res);
        STORE_REG(op.reg[2], res);
  }
};


struct BinarySubscrDict: public RegOpImpl<RegOp<3>, BinarySubscrDict> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    PyObject* dict = LOAD_OBJ(op.reg[0]);
    PyObject* key = LOAD_OBJ(op.reg[1]);

    CHECK_VALID(dict);
    CHECK_VALID(key);

    PyObject* res = PyDict_GetItem(dict, key);

    if (res != 0) {
      Py_INCREF(res);
      CHECK_VALID(res);
      STORE_REG(op.reg[2], res);
      return;
    }
    res = PyObject_GetItem(dict, key);
    if (!res) {
     throw RException();
    }
    CHECK_VALID(res);
    STORE_REG(op.reg[2], res);
  }
};

struct InplacePower: public RegOpImpl<RegOp<3>, InplacePower> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    PyObject* r1 = LOAD_OBJ(op.reg[0]);
    CHECK_VALID(r1);
    PyObject* r2 = LOAD_OBJ(op.reg[1]);
    CHECK_VALID(r2);
    PyObject* r3 = PyNumber_Power(r1, r2, Py_None);
    if (!r3) {
      throw RException();
    }
    STORE_REG(op.reg[2], r3);
  }
};


#define Py3kExceptionClass_Check(x)     \
    (PyType_Check((x)) &&               \
     PyType_FastSubclass((PyTypeObject*)(x), Py_TPFLAGS_BASE_EXC_SUBCLASS))

#define CANNOT_CATCH_MSG "catching classes that don't inherit from " \
                         "BaseException is not allowed in 3.x"

/* slow path for comparisons, copied from ceval */
static f_inline PyObject* cmp_outcome(int op, PyObject *v, PyObject *w) {
    int res = 0;
    switch (op) {
    case PyCmp_IS:
        res = (v == w);
        break;
    case PyCmp_IS_NOT:
        res = (v != w);
        break;
    case PyCmp_IN:
        res = PySequence_Contains(w, v);
        if (res < 0)
            return NULL;
        break;
    case PyCmp_NOT_IN:
        res = PySequence_Contains(w, v);
        if (res < 0)
            return NULL;
        res = !res;
        break;
    case PyCmp_EXC_MATCH:
        if (PyTuple_Check(w)) {
            Py_ssize_t i, length;
            length = PyTuple_Size(w);
            for (i = 0; i < length; i += 1) {
                PyObject *exc = PyTuple_GET_ITEM(w, i);
                if (PyString_Check(exc)) {
                    int ret_val;
                    ret_val = PyErr_WarnEx(
                        PyExc_DeprecationWarning,
                        "catching of string "
                        "exceptions is deprecated", 1);
                    if (ret_val < 0)
                        return NULL;
                }
                else if (Py_Py3kWarningFlag  &&
                         !PyTuple_Check(exc) &&
                         !Py3kExceptionClass_Check(exc))
                {
                    int ret_val;
                    ret_val = PyErr_WarnEx(
                        PyExc_DeprecationWarning,
                        CANNOT_CATCH_MSG, 1);
                    if (ret_val < 0)
                        return NULL;
                }
            }
        }
        else {
            if (PyString_Check(w)) {
                int ret_val;
                ret_val = PyErr_WarnEx(
                                PyExc_DeprecationWarning,
                                "catching of string "
                                "exceptions is deprecated", 1);
                if (ret_val < 0)
                    return NULL;
            }
            else if (Py_Py3kWarningFlag  &&
                     !PyTuple_Check(w) &&
                     !Py3kExceptionClass_Check(w))
            {
                int ret_val;
                ret_val = PyErr_WarnEx(
                    PyExc_DeprecationWarning,
                    CANNOT_CATCH_MSG, 1);
                if (ret_val < 0)
                    return NULL;
            }
        }
        res = PyErr_GivenExceptionMatches(v, w);
        break;
    default:
        return PyObject_RichCompare(v, w, op);
    }
    v = res ? Py_True : Py_False;
    Py_INCREF(v);
    return v;
}

struct CompareOp: public RegOpImpl<RegOp<3>, CompareOp> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    Register& r1 = registers[op.reg[0]];
    Register& r2 = registers[op.reg[1]];
    PyObject* r3 = NULL;
    if (r1.get_type() == IntType && r2.get_type() == IntType) {
      r3 = IntegerOps::compare(r1.as_int(), r2.as_int(), op.arg);
    } /* else {
      r3 = FloatOps::compare(r1.as_obj(), r2.as_obj(), op.arg);
    }
    */
    if (r3 != NULL) {
      Py_INCREF(r3);
    } else {
      // r3 = PyObject_RichCompare(r1.as_obj(), r2.as_obj(), op.arg);
      r3 = cmp_outcome(op.arg, r1.as_obj(), r2.as_obj());
    }
    if (!r3) {
      throw RException();
    }

    STORE_REG(op.reg[2], r3);
  }
};

struct DictContains : public RegOpImpl<RegOp<3>, DictContains> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    PyObject* dict = LOAD_OBJ(op.reg[0]);
    CHECK_VALID(dict);

    PyObject* elt = LOAD_OBJ(op.reg[1]);
    CHECK_VALID(elt);

    int result_code = PyDict_Contains(dict, elt);
    if (result_code == -1) {
      result_code = PySequence_Contains(dict, elt);
      if (result_code == -1) {
        throw RException();
      }
    }
    PyObject* result = result_code ? Py_True : Py_False;
    Py_INCREF(result);
    STORE_REG(op.reg[2], result);
  }
};

struct IncRef: public RegOpImpl<RegOp<1>, IncRef> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
    CHECK_VALID(LOAD_OBJ(op.reg[0]));
    Py_INCREF(LOAD_OBJ(op.reg[0]));
  }
};

struct DecRef: public RegOpImpl<RegOp<1>, DecRef> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
    CHECK_VALID(LOAD_OBJ(op.reg[0]));
    Py_DECREF(LOAD_OBJ(op.reg[0]));
  }
};

struct LoadLocals: public RegOpImpl<RegOp<1>, LoadLocals> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
    Py_INCREF(frame->locals());
    STORE_REG(op.reg[0], frame->locals());
  }
};

struct LoadGlobal: public RegOpImpl<RegOp<1>, LoadGlobal> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
    PyObject* key = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyObject* value = PyDict_GetItem(frame->globals(), key);
    if (value != NULL) {
      Py_INCREF(value);
      STORE_REG(op.reg[0], value);
      return;
    }
    value = PyDict_GetItem(frame->builtins(), key);
    if (value != NULL) {
      Py_INCREF(value);
      STORE_REG(op.reg[0], value);
      return;
    }
    throw RException(PyExc_NameError, "Global name %.200s not defined.", obj_to_str(key));
  }
};

struct StoreGlobal: public RegOpImpl<RegOp<1>, StoreGlobal> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
    PyObject* key = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyObject* val = LOAD_OBJ(op.reg[0]);
    PyDict_SetItem(frame->globals(), key, val);
  }
};

struct DeleteGlobal: public RegOpImpl<RegOp<0>, DeleteGlobal> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<0>& op, Register* registers) {
    PyObject* key = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyDict_DelItem(frame->globals(), key);
  }
};

struct LoadName: public RegOpImpl<RegOp<1>, LoadName> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
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

    STORE_REG(op.reg[0], r2);
  }
};

struct StoreName: public RegOpImpl<RegOp<1>, StoreName> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
    PyObject* r1 = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyObject* r2 = LOAD_OBJ(op.reg[0]);
    CHECK_VALID(r1);
    CHECK_VALID(r2);
    PyObject_SetItem(frame->locals(), r1, r2);
  }
};

// LOAD_FAST and STORE_FAST both perform the same operation in the register VM.
struct LoadFast: public RegOpImpl<RegOp<2>, LoadFast> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, Register* registers) {
    Register& a = registers[op.reg[0]];
    Register& b = registers[op.reg[1]];
    a.incref();
    b.decref();
    b.store(a);
  }
};
typedef LoadFast StoreFast;

struct StoreAttr: public RegOpImpl<RegOp<2>, StoreAttr> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, Register* registers) {
    PyObject* obj = LOAD_OBJ(op.reg[0]);
    PyObject* key = PyTuple_GET_ITEM(frame->names(), op.arg);
    PyObject* value = LOAD_OBJ(op.reg[1]);
    CHECK_VALID(obj);
    CHECK_VALID(key);
    CHECK_VALID(value);
    if (PyObject_SetAttr(obj, key, value) != 0) {
      throw RException();
    }
  }
};



struct StoreSubscr: public RegOpImpl<RegOp<3>, StoreSubscr> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    PyObject* key = LOAD_OBJ(op.reg[0]);
    PyObject* list = LOAD_OBJ(op.reg[1]);
    PyObject* value = LOAD_OBJ(op.reg[2]);
    CHECK_VALID(key);
    CHECK_VALID(list);
    CHECK_VALID(value);
    if (PyObject_SetItem(list, key, value) != 0) {
      throw RException();
    }
  }
};


struct StoreSubscrList: public RegOpImpl<RegOp<3>, StoreSubscrList> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    PyObject* list = LOAD_OBJ(op.reg[1]);
    PyObject* value = LOAD_OBJ(op.reg[2]);
    CHECK_VALID(list);
    CHECK_VALID(value);
    Register& idx_reg = registers[op.reg[0]];
    if (idx_reg.get_type() != IntType) {
      PyObject* idx_obj = LOAD_OBJ(op.reg[0]);
      CHECK_VALID(idx_obj);
      if (PyObject_SetItem(list, idx_obj, value) != 0) {
        throw RException();
      }
    } else {
      Py_ssize_t idx = idx_reg.as_int();
      if (PyList_SetItem(list, idx, value) != 0) {
          throw RException();
      }
    }
  }
};

struct StoreSubscrDict: public RegOpImpl<RegOp<3>, StoreSubscrDict> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    PyObject* key = LOAD_OBJ(op.reg[0]);
    PyObject* list = LOAD_OBJ(op.reg[1]);
    PyObject* value = LOAD_OBJ(op.reg[2]);
    CHECK_VALID(key);
    CHECK_VALID(list);
    CHECK_VALID(value);
    if (PyDict_SetItem(list, key, value) != 0) {
      throw RException();
    }
  }
};

// Copied from ceval.cc:
#define ISINDEX(x) ((x) == NULL || PyInt_Check(x) || PyLong_Check(x) || PyIndex_Check(x))
static int assign_slice(PyObject *u, PyObject *v, PyObject *w, PyObject *x) {
  PyTypeObject *tp = u->ob_type;
  PySequenceMethods *sq = tp->tp_as_sequence;

  if (sq && sq->sq_ass_slice && ISINDEX(v) && ISINDEX(w)) {
    Py_ssize_t ilow = 0, ihigh = PY_SSIZE_T_MAX;
    if (!_PyEval_SliceIndex(v, &ilow)) return -1;
    if (!_PyEval_SliceIndex(w, &ihigh)) return -1;
    if (x == NULL) return PySequence_DelSlice(u, ilow, ihigh);
    else return PySequence_SetSlice(u, ilow, ihigh, x);
  } else {
    PyObject *slice = PySlice_New(v, w, NULL);
    if (slice != NULL) {
      int res;
      if (x != NULL) res = PyObject_SetItem(u, slice, x);
      else res = PyObject_DelItem(u, slice);
      Py_DECREF(slice);
      return res;
    } else return -1;
  }
}

struct StoreSlice: public RegOpImpl<RegOp<4>, StoreSlice> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<4>& op, Register* registers) {
    PyObject* list = LOAD_OBJ(op.reg[0]);
    PyObject* left = op.reg[1] != kInvalidRegister ? LOAD_OBJ(op.reg[1]) : NULL;
    PyObject* right = op.reg[2] != kInvalidRegister ? LOAD_OBJ(op.reg[2]) : NULL;
    PyObject* value = LOAD_OBJ(op.reg[3]);
    if (assign_slice(list, left, right, value) != 0) {
      throw RException();
    }
  }
};

struct ConstIndex: public RegOpImpl<RegOp<2>, ConstIndex> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, Register* registers) {
    PyObject* list = LOAD_OBJ(op.reg[0]);
    uint8_t key = op.arg;
    if (op.reg[1] == kInvalidRegister) {
      return;
    }

    PyObject* pykey = PyInt_FromLong(key);
    STORE_REG(op.reg[1], PyObject_GetItem(list, pykey));
    Py_DECREF(pykey);
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
      Reg_AssertGt(dictoffset, 0);
      Reg_AssertEq(dictoffset % SIZEOF_VOID_P, 0);
    }
    dictptr = (PyObject **) ((char *) obj + dictoffset);
    return (PyDictObject*) *dictptr;
  }
  return NULL;
}

static size_t dict_getoffset(PyDictObject* dict, PyObject* key) {
  long hash;
  if (!PyString_CheckExact(key) || (hash = ((PyStringObject *) key)->ob_shash) == -1) {
    hash = PyObject_Hash(key);
  }

  PyDictEntry* pos = dict->ma_lookup(dict, key, hash);
  return pos - dict->ma_table;
}

// LOAD_ATTR is common enough to warrant inlining some common code.
// Most of this is taken from _PyObject_GenericGetAttrWithDict
static PyObject * obj_getattr(Evaluator* eval, RegOp<2>& op, PyObject *obj, PyObject *name) {
  PyObjHelper<PyTypeObject*> type(Py_TYPE(obj) );
  PyObjHelper<PyDictObject*> dict(obj_getdictptr(obj, type));
  PyObject *descr = NULL;
#if GETATTR_HINTS
  const Hint &op_hint = eval->hints[op.hint_pos];

  // A hint for an instance dictionary lookup.
  if (dict.val_ && op_hint.guard.dict_size == dict->ma_mask) {
    const PyDictEntry &e(dict->ma_table[op_hint.version]);
    if (e.me_key == name) {
      Py_INCREF(e.me_value);
      return e.me_value;
    }
  }
#endif

  if (!PyString_Check(name)) {
    throw RException(PyExc_SystemError, "attribute name must be string, not '%.200s'", Py_TYPE(name) ->tp_name);
  }

  if (type->tp_dict == NULL) {
    if (PyType_Ready(type) < 0) {
      throw RException();
    }
  }

  // A hint for a type dictionary lookup, we've cached the 'descr' object.
//  if (op_hint.guard.obj == type && op_hint.key == name && op_hint.version == type->tp_version_tag) {
//    descr = op_hint.value;
//  } else {
  descr = _PyType_Lookup(type, name);
//  }

  descrgetfunc getter = NULL;
  if (descr != NULL && PyType_HasFeature(descr->ob_type, Py_TPFLAGS_HAVE_CLASS)) {
    getter = descr->ob_type->tp_descr_get;
    if (getter != NULL && PyDescr_IsData(descr)) {
      return getter(descr, obj, (PyObject*) type);
    }
  }

  // Look for a match in our object dictionary
  if (dict != NULL) {
    PyObject* res = PyDict_GetItem(dict, name);
    // We found a match.  Create a hint for where to look next time.
    if (res != NULL) {
#if GETATTR_HINTS
      size_t hint_pos = hint_offset(type, name);
      size_t dict_pos = dict_getoffset(dict, name);

      Hint h;
      h.guard.dict_size = dict->ma_mask;
      h.key = name;
      h.value = type;
      h.version = dict_pos;
      eval->hints[hint_pos] = h;
      op.hint_pos = hint_pos;
#endif
      Py_INCREF(res);
      return res;
    }
  }

  // Instance dictionary lookup failed, try to find a match in the class hierarchy.
  if (getter != NULL) {
    PyObject* res = getter(descr, obj, (PyObject*) type);
    if (res != NULL) {
//      size_t hint_pos = hint_offset(type, name);
//      Hint h;
//      h.guard.obj = type;
//      h.key = name;
//      h.value = descr;
//      h.version = type->tp_version_tag;
//      eval->hints[hint_pos] = h;
//      op.hint_pos = hint_pos;

      return res;
    }
  }

  if (descr != NULL) {
    return descr;
  }

  throw RException(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", type->tp_name,
                   PyString_AS_STRING(name) );
}

struct LoadAttr: public RegOpImpl<RegOp<2>, LoadAttr> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, Register* registers) {
    PyObject* obj = LOAD_OBJ(op.reg[0]);
    PyObject* name = PyTuple_GET_ITEM(frame->names(), op.arg);
    PyObject* res = obj_getattr(eval, op, obj, name);
    STORE_REG(op.reg[1], res);
//    Py_INCREF(LOAD_OBJ(op.reg[1]));
      }
    };

struct LoadDeref: public RegOpImpl<RegOp<1>, LoadDeref> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
    PyObject* closure_cell = frame->freevars[op.arg];
    PyObject* closure_value = PyCell_Get(closure_cell);
    STORE_REG(op.reg[0], closure_value);
  }
};

struct StoreDeref: public RegOpImpl<RegOp<1>, StoreDeref> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
    PyObject* value = LOAD_OBJ(op.reg[0]);
    PyObject* dest_cell = frame->freevars[op.arg];
    PyCell_Set(dest_cell, value);
  }
};

struct LoadClosure: public RegOpImpl<RegOp<1>, LoadClosure> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
    PyObject* closure_cell = frame->freevars[op.arg];
    Py_INCREF(closure_cell);
    STORE_REG(op.reg[0], closure_cell);
  }
};

struct MakeFunction: public VarArgsOpImpl<MakeFunction> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, VarRegOp *op, Register* registers) {
    PyObject* code = LOAD_OBJ(op->reg[0]);
    PyObject* func = PyFunction_New(code, frame->globals());
    PyObject* defaults = PyTuple_New(op->arg);
    for (int i = 0; i < op->arg; ++i) {
      PyTuple_SetItem(defaults, i, LOAD_OBJ(op->reg[i + 1]));
    }
    PyFunction_SetDefaults(func, defaults);
    STORE_REG(op->reg[op->arg + 1], func);
  }
};

struct MakeClosure: public VarArgsOpImpl<MakeClosure> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, VarRegOp *op, Register* registers) {
    // first register argument is the code object
    // second is the closure args tuple
    // rest of the registers are default argument values
    PyObject* code = LOAD_OBJ(op->reg[0]);
    PyObject* func = PyFunction_New(code, frame->globals());
    PyObject* closure_values = LOAD_OBJ(op->reg[1]);
    PyFunction_SetClosure(func, closure_values);

    PyObject* defaults = PyTuple_New(op->arg);
    for (int i = 0; i < op->arg; ++i) {
      PyTuple_SetItem(defaults, i, LOAD_OBJ(op->reg[i + 2]));
    }
    PyFunction_SetDefaults(func, defaults);
    STORE_REG(op->reg[op->arg + 2], func);
  }
};

template <bool HasVarArgs, bool HasKwDict>
struct CallFunction: public VarArgsOpImpl<CallFunction<HasVarArgs, HasKwDict> > {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, VarRegOp *op, Register* registers) {
    int na = op->arg & 0xff;
    int nk = (op->arg >> 8) & 0xff;
    int n = nk * 2 + na;

    if (HasVarArgs) n++;
    if (HasKwDict) n++;

    int dst = op->reg[n + 1];

    PyObject* fn = LOAD_OBJ(op->reg[0]);

    Reg_AssertEq(n + 2, op->num_registers);

    RegisterCode* code = NULL;

    /* TODO:
     *   Actually accelerate object construction in Falcon by
     *   first creating the raw/unitialized object and then
     *   compiling the __init__ method of the called class.
     *
     *   To actually get a performance gain from this we would need
     *   special instance dictionaries which store Falcon registers
     *   and only lazily construct PyObject representations when asked
     *   by other Python C API code.
     */
    if (!PyCFunction_Check(fn) && !PyClass_Check(fn)) {
      try {
        code = eval->compile(fn);
      } catch (RException& e) {
        Log_Info("Failed to compile function, executing using ceval: %s", obj_to_str(e.value));
        code = NULL;
      }
    }

    if (code == NULL || nk > 0) {
      PyObject* args = PyTuple_New(na);

      for (register int i = 0; i < na; ++i) {
        PyObject* v = LOAD_OBJ(op->reg[i+1]);
        Py_INCREF(v);
        PyTuple_SET_ITEM(args, i, v);
      }

      PyObject* kwdict = NULL;
      if (nk > 0) {
        kwdict = PyDict_New();
        for (register int i = na; i < nk * 2 ; i += 2) {
          // starting at +1 since the first register was the fn
          // so keyword args actually start at na+1
          PyObject* k = LOAD_OBJ(op->reg[i+1]);
          PyObject* v = LOAD_OBJ(op->reg[i+2]);
          PyDict_SetItem(kwdict, k, v);
        }
      }

      PyObject* res = NULL;
      if (PyCFunction_Check(fn)) {
        res = PyCFunction_Call(fn, args, kwdict);
      } else {
        res = PyObject_Call(fn, args, kwdict);
      }
      Py_DECREF(args);

      if (res == NULL) {
        throw RException();
      }

      STORE_REG(dst, res);
    } else {
      ObjVector args, kw;
      args.resize(na);
      for (register int i = 0; i < na; ++i) {
        args[i].store(registers[op->reg[i+1]]);
      }
      RegisterFrame f(code, fn, args, kw);
      STORE_REG(dst, eval->eval(&f));
    }
  }
};

typedef CallFunction<false,false> CallFunctionSimple;
typedef CallFunction<true,false> CallFunctionVar;
typedef CallFunction<false,true> CallFunctionKw;
typedef CallFunction<true,true> CallFunctionVarKw;

struct GetIter: public RegOpImpl<RegOp<2>, GetIter> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, Register* registers) {
    PyObject* res = PyObject_GetIter(LOAD_OBJ(op.reg[0]));
    STORE_REG(op.reg[1], res);
  }
};

struct ForIter: public BranchOpImpl<BranchOp<2>, ForIter> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *frame, BranchOp<2>& op, const char **pc, Register* registers) {
    CHECK_VALID(LOAD_OBJ(op.reg[0]));
    PyObject* iter = PyIter_Next(LOAD_OBJ(op.reg[0]));
    if (iter) {
      STORE_REG(op.reg[1], iter);
      *pc += sizeof(BranchOp<2>);
    } else {
      *pc = frame->instructions() + op.label;
    }

  }
};

struct JumpIfFalseOrPop: public BranchOpImpl<BranchOp<1>, JumpIfFalseOrPop> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *frame, BranchOp<1>& op, const char **pc, Register* registers) {
    PyObject *r1 = LOAD_OBJ(op.reg[0]);
    if (r1 == Py_False || (PyObject_IsTrue(r1) == 0)) {
//      EVAL_LOG("Jumping: %s -> %d", obj_to_str(r1), op.label);
        *pc = frame->instructions() + op.label;
      } else {
        *pc += sizeof(BranchOp<1>);
      }

    }
  };

struct JumpIfTrueOrPop: public BranchOpImpl<BranchOp<1>, JumpIfTrueOrPop> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *frame, BranchOp<1>& op, const char **pc, Register* registers) {
    PyObject* r1 = LOAD_OBJ(op.reg[0]);
    if (r1 == Py_True || (PyObject_IsTrue(r1) == 1)) {
      *pc = frame->instructions() + op.label;
    } else {
      *pc += sizeof(BranchOp<1>);
    }

  }
};

struct JumpAbsolute: public BranchOpImpl<BranchOp<0>, JumpAbsolute> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *frame, BranchOp<0>& op, const char **pc, Register* registers) {
    EVAL_LOG("Jumping to: %d", op.label);
    *pc = frame->instructions() + op.label;
  }
};

struct BreakLoop: public BranchOpImpl<BranchOp<0>, BreakLoop> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame *frame, BranchOp<0>& op, const char **pc, Register* registers) {
    EVAL_LOG("Jumping to: %d", op.label);
    *pc = frame->instructions() + op.label;
  }
};

// Evaluation of RETURN_VALUE is special.  g++ exceptions are excrutiatingly slow, so we
// can't use the exception mechanism to jump to our exit point.  Instead, we return a value
// here and jump to the exit of our frame.
struct ReturnValue {
  static f_inline Register* eval(Evaluator* eval, RegisterFrame* frame, const char* pc, Register* registers) {
    RegOp<1>& op = *((RegOp<1>*) pc);
    EVAL_LOG("%s -- %5d: %s", frame->str().c_str(), frame->offset(pc), op.str(registers).c_str());
    Register& r = registers[op.reg[0]];
    r.incref();
    return &r;
  }
};

struct Nop: public RegOpImpl<RegOp<0>, Nop> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<0>& op, Register* registers) {

  }
};

struct BuildTuple: public VarArgsOpImpl<BuildTuple> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, VarRegOp *op, Register* registers) {
    register int count = op->arg;
    PyObject* t = PyTuple_New(count);
    for (register int i = 0; i < count; ++i) {
      PyObject* v = LOAD_OBJ(op->reg[i]);
      Py_INCREF(v);
      PyTuple_SET_ITEM(t, i, v);
    }
    STORE_REG(op->reg[count], t);
  }
};

struct BuildList: public VarArgsOpImpl<BuildList> {
  static f_inline void _eval(Evaluator* eval, RegisterFrame* frame, VarRegOp *op, Register* registers) {
    register int count = op->arg;
    PyObject* t = PyList_New(count);
    for (register int i = 0; i < count; ++i) {
      PyObject* v = LOAD_OBJ(op->reg[i]);
      Py_INCREF(v);
      PyList_SET_ITEM(t, i, v);
    }
    STORE_REG(op->reg[count], t);
  }
};

struct BuildMap: public RegOpImpl<RegOp<1>, BuildMap> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
    // for now ignore the size hint in the op arg
    PyObject* dict = PyDict_New();
    STORE_REG(op.reg[0], dict);
  }
};

struct BuildSlice: public RegOpImpl<RegOp<4>, BuildSlice> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<4>& op, Register* registers) {
    PyObject* w = LOAD_OBJ(op.reg[0]);
    PyObject* v = LOAD_OBJ(op.reg[1]);
    PyObject* u = LOAD_OBJ(op.reg[2]);

    STORE_REG(op.reg[3], PySlice_New(u, v, w))
  }
};

struct BuildClass: public RegOpImpl<RegOp<4>, BuildClass> {
  static void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<4>& op, Register* registers) {
    PyObject* methods = LOAD_OBJ(op.reg[0]);
    PyObject* bases = LOAD_OBJ(op.reg[1]);
    PyObject* name = LOAD_OBJ(op.reg[2]);

    // Begin: build_class from ceval.c
        PyObject *metaclass = NULL, *result, *base;

        if (PyDict_Check(methods)) metaclass = PyDict_GetItemString(methods, "__metaclass__");
        if (metaclass != NULL)
        Py_INCREF(metaclass);
        else if (PyTuple_Check(bases) && PyTuple_GET_SIZE(bases) > 0) {
          base = PyTuple_GET_ITEM(bases, 0);
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
        STORE_REG(op.reg[3], result);
      }
    };

struct PrintItem: public RegOpImpl<RegOp<2>, PrintItem> {
  static void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, Register* registers) {
    PyObject* v = LOAD_OBJ(op.reg[0]);
    PyObject* w = op.reg[1] != kInvalidRegister ? LOAD_OBJ(op.reg[1]) : PySys_GetObject((char*) "stdout");

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
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
    PyObject* w = op.reg[0] != kInvalidRegister ? LOAD_OBJ(op.reg[0]): PySys_GetObject((char*) "stdout");
    int err = PyFile_WriteString("\n", w);
    if (err == 0) PyFile_SoftSpace(w, 0);
  }
};

struct ListAppend: public RegOpImpl<RegOp<2>, ListAppend> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<2>& op, Register* registers) {
    PyList_Append(LOAD_OBJ(op.reg[0]), LOAD_OBJ(op.reg[1]));
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
    }
    return NULL;
  }
}

struct Slice: public RegOpImpl<RegOp<4>, Slice> {
  static f_inline void _eval(Evaluator *eval, RegisterFrame* frame, RegOp<4>& op, Register* registers) {
    PyObject* list = LOAD_OBJ(op.reg[0]);
    PyObject* left = op.reg[1] != kInvalidRegister ? LOAD_OBJ(op.reg[1]) : NULL;
    PyObject* right = op.reg[2] != kInvalidRegister ? LOAD_OBJ(op.reg[2]) : NULL;
    PyObject* result = apply_slice(list, left, right);
    if (!result) {
      throw RException();
    }
    STORE_REG(op.reg[3], result);
  }
};

template<int Opcode>
struct BadOp {
  static n_inline void eval(Evaluator *eval, RegisterFrame* frame, Register* registers) {
    const char* name = OpUtil::name(Opcode);
    throw RException(PyExc_SystemError, "Bad opcode %s", name);
  }
};

// Imports

struct ImportName: public RegOpImpl<RegOp<3>, ImportName> {
  static void _eval(Evaluator* eval, RegisterFrame* frame, RegOp<3>& op, Register* registers) {
    PyObject* name = PyTuple_GET_ITEM(frame->names(), op.arg) ;
    PyObject* import = PyDict_GetItemString(frame->builtins(), "__import__");
    if (import == NULL) {
      throw RException(PyExc_ImportError, "__import__ not found in builtins.");
    }

    PyObject* args = NULL;
    PyObject* v = LOAD_OBJ(op.reg[0]);
    PyObject* u = LOAD_OBJ(op.reg[1]);
    if (PyInt_AsLong(u) != -1 || PyErr_Occurred()) {
      PyErr_Clear();
      args = PyTuple_Pack(5, name, frame->globals(), frame->locals(), v, u);
    } else {
      args = PyTuple_Pack(4, name, frame->globals(), frame->locals(), v);
    }

    PyObject* res = PyEval_CallObject(import, args);
    STORE_REG(op.reg[2], res);
  }
};

struct ImportStar: public RegOpImpl<RegOp<1>, ImportStar> {
  static void _eval(Evaluator* eval, RegisterFrame* frame, RegOp<1>& op, Register* registers) {
    PyObject* module = LOAD_OBJ(op.reg[0]);
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
  static void _eval(Evaluator* eval, RegisterFrame* frame, RegOp<2>& op, Register* registers) {
    PyObject* name = PyTuple_GetItem(frame->names(), op.arg);
    PyObject* module = LOAD_OBJ(op.reg[0]);
    Py_XDECREF(LOAD_OBJ(op.reg[1]));
    PyObject* val = PyObject_GetAttr(module, name);
    if (val == NULL) {
      if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        throw RException(PyExc_ImportError, "cannot import name %.230s", PyString_AsString(name));
      } else {
        throw RException();
      }
    }

    STORE_REG(op.reg[1], val);
  }
};

#define CONCAT(...) __VA_ARGS__

#define REGISTER_OP(opname)\
    static int _force_register_ ## opname = LabelRegistry::add_label(opname, &&op_ ## opname);

#define JUMP_TO(opname)\
    goto *labels[opname]

#define _DEFINE_OP(opname, impl)\
      /*collectInfo(opname);\*/\
      pc = impl::eval(this, frame, pc, registers);\
      JUMP_TO(frame->next_code(pc));

#define DEFINE_OP(opname, impl)\
    op_##opname:\
      _DEFINE_OP(opname, impl)

#define BAD_OP(opname)\
    op_##opname:\
     BadOp<opname>::eval(this, frame, registers);

#define FALLTHROUGH(opname) op_##opname:

#define BINARY_OP3(opname, objfn, intfn, can_overflow)\
    op_##opname: _DEFINE_OP(opname, BinaryOpWithSpecialization<CONCAT(opname, objfn, intfn, can_overflow)>)

#define BINARY_OP2(opname, objfn)\
    op_##opname: _DEFINE_OP(opname, BinaryOp<CONCAT(opname, objfn)>)

#define UNARY_OP2(opname, objfn)\
    op_##opname: _DEFINE_OP(opname, UnaryOp<CONCAT(opname, objfn)>)

Register Evaluator::eval(RegisterFrame* f) {
  register RegisterFrame* frame = f;
  register Register* registers asm("r15") = frame->registers;
  register const char* pc asm("r14") = frame->instructions();

  Reg_Assert(frame != NULL, "NULL frame object.");
  // Reg_Assert(PyTuple_GET_SIZE(frame->code->code()->co_cellvars) == 0, "Cell vars (closures) not supported.");

  Register* result;

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
  OFFSET(CONST_INDEX),
  OFFSET(BINARY_SUBSCR_LIST),
  OFFSET(BINARY_SUBSCR_DICT),
  OFFSET(DICT_CONTAINS),
  OFFSET(STORE_SUBSCR_LIST),
  OFFSET(STORE_SUBSCR_DICT),
}
;

//EVAL_LOG("Entering frame: %s", frame->str().c_str());
try {
    JUMP_TO(frame->next_code(pc));

op_RETURN_VALUE: {
  result = ReturnValue::eval(this, frame, pc, registers);
  goto done;
}

op_BADCODE: {
  EVAL_LOG("Jump to invalid opcode!?");
throw RException(PyExc_SystemError, "Invalid jump.");
}
BAD_OP(STOP_CODE);

BINARY_OP3(BINARY_MULTIPLY, PyNumber_Multiply, IntegerOps::mul, true);
BINARY_OP3(BINARY_DIVIDE, PyNumber_Divide, IntegerOps::div, true);
BINARY_OP3(BINARY_ADD, PyNumber_Add, IntegerOps::add, true);
BINARY_OP3(BINARY_SUBTRACT, PyNumber_Subtract, IntegerOps::sub, true);
BINARY_OP3(BINARY_OR, PyNumber_Or, IntegerOps::Or, false);
BINARY_OP3(BINARY_XOR, PyNumber_Xor, IntegerOps::Xor, false);
BINARY_OP3(BINARY_AND, PyNumber_And, IntegerOps::And, false);
BINARY_OP3(BINARY_RSHIFT, PyNumber_Rshift, IntegerOps::Rshift, false);
BINARY_OP3(BINARY_LSHIFT, PyNumber_Lshift, IntegerOps::Lshift, false);
BINARY_OP2(BINARY_TRUE_DIVIDE, PyNumber_TrueDivide);
BINARY_OP2(BINARY_FLOOR_DIVIDE, PyNumber_FloorDivide);

DEFINE_OP(BINARY_POWER, BinaryPower);
DEFINE_OP(BINARY_MODULO, BinaryModulo);

DEFINE_OP(BINARY_SUBSCR, BinarySubscr);
DEFINE_OP(BINARY_SUBSCR_LIST, BinarySubscrList);
DEFINE_OP(BINARY_SUBSCR_DICT, BinarySubscrDict);
DEFINE_OP(CONST_INDEX, ConstIndex);


DEFINE_OP(DICT_CONTAINS, DictContains);

BINARY_OP3(INPLACE_MULTIPLY, PyNumber_InPlaceMultiply, IntegerOps::mul, true);
BINARY_OP3(INPLACE_DIVIDE, PyNumber_InPlaceDivide, IntegerOps::div, true);
BINARY_OP3(INPLACE_ADD, PyNumber_InPlaceAdd, IntegerOps::add, true);
BINARY_OP3(INPLACE_SUBTRACT, PyNumber_InPlaceSubtract, IntegerOps::sub, true);
BINARY_OP3(INPLACE_MODULO, PyNumber_InPlaceRemainder, IntegerOps::mod, true);

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
DEFINE_OP(STORE_SUBSCR_LIST, StoreSubscrList);
DEFINE_OP(STORE_SUBSCR_DICT, StoreSubscrDict);

DEFINE_OP(STORE_FAST, StoreFast);
DEFINE_OP(STORE_SLICE, StoreSlice);

DEFINE_OP(LOAD_GLOBAL, LoadGlobal);
DEFINE_OP(STORE_GLOBAL, StoreGlobal);
DEFINE_OP(DELETE_GLOBAL, DeleteGlobal);

DEFINE_OP(LOAD_CLOSURE, LoadClosure);
DEFINE_OP(LOAD_DEREF, LoadDeref);
DEFINE_OP(STORE_DEREF, StoreDeref);


DEFINE_OP(GET_ITER, GetIter);
DEFINE_OP(FOR_ITER, ForIter);
DEFINE_OP(BREAK_LOOP, BreakLoop);

DEFINE_OP(BUILD_TUPLE, BuildTuple);
DEFINE_OP(BUILD_LIST, BuildList);
DEFINE_OP(BUILD_MAP, BuildMap);
DEFINE_OP(BUILD_SLICE, BuildSlice);

DEFINE_OP(PRINT_NEWLINE, PrintNewline);
DEFINE_OP(PRINT_NEWLINE_TO, PrintNewline);
DEFINE_OP(PRINT_ITEM, PrintItem);
DEFINE_OP(PRINT_ITEM_TO, PrintItem);

DEFINE_OP(CALL_FUNCTION, CallFunctionSimple);
DEFINE_OP(CALL_FUNCTION_VAR, CallFunctionVar);
DEFINE_OP(CALL_FUNCTION_KW, CallFunctionKw);
DEFINE_OP(CALL_FUNCTION_VAR_KW, CallFunctionVarKw);

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
BAD_OP(RAISE_VARARGS);
BAD_OP(DELETE_FAST);
BAD_OP(SETUP_FINALLY);
BAD_OP(SETUP_EXCEPT);
BAD_OP(CONTINUE_LOOP);
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
BAD_OP(NOP);
BAD_OP(ROT_FOUR);
BAD_OP(DUP_TOP);
BAD_OP(ROT_THREE);
BAD_OP(ROT_TWO);
BAD_OP(POP_TOP);

} catch (RException &error) {
//    EVAL_LOG("ERROR: Leaving frame: %s", frame->str().c_str());
if (error.exception != NULL) {
  PyErr_SetObject(error.exception, error.value);
}

// TODO(power) - create a frame object here and attach it to the traceback.
PyFrameObject* py_frame = PyFrame_New(PyThreadState_GET(), frame->code->code(), frame->globals(), frame->locals());
PyTraceBack_Here(py_frame);
return Register(NULL);
}
done: {
//    EVAL_LOG("SUCCESS: Leaving frame: %s", frame->str().c_str());
return *result;
}
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
