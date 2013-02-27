#ifndef REVAL_H_
#define REVAL_H_

#include "frameobject.h"
#include "util.h"

#include "rinst.h"
#include "rexcept.h"
#include "rcompile.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include <boost/noncopyable.hpp>


// A vector which we can normally stack allocate, and which
// contains a small number of slots internally.
static const size_t kSVBuiltinSlots = 8;
template<class T>
struct SmallVector {
  SmallVector() :
      limit_(kSVBuiltinSlots), count_(0), rest_(NULL) {
  }
  ~SmallVector() {
    if (rest_) {
      free(rest_);
    }
  }

  void ensure(size_t needed) {
    if (limit_ > needed) {
      return;
    }

    if (limit_ < needed) {
      rest_ = (T*)realloc(rest_, sizeof(T) * needed * 2);
      limit_ = kSVBuiltinSlots + needed * 2;
    }
  }

  T& operator[](const size_t idx) const {
    if (idx < kSVBuiltinSlots) {
      return (T&)(vals_[idx]);
    }
    return (T&)(rest_[idx - kSVBuiltinSlots]);
  }

  T& at(const size_t idx) {
    return (*this)[idx];
  }

  void push_back(const T& t) {
    if (count_ < kSVBuiltinSlots) {
      vals_[count_++] = t;
    } else {
      ensure(count_ + 1);
      rest_[count_ - kSVBuiltinSlots] = t;
      ++count_;
    }
  }

  size_t size() const {
    return count_;
  }

  bool empty() const {
    return count_ == 0;
  }

  size_t limit_;
  size_t count_;
  T vals_[kSVBuiltinSlots];
  T* rest_;
};

typedef SmallVector<PyObject*> ObjVector;

struct Hint {
  PyObject* obj;
  PyObject* key;
  PyObject* value;
  unsigned int version;
};

struct RegisterFrame: private boost::noncopyable {
private:
  PyObject* builtins_;
  PyObject* globals_;
  PyObject* locals_;
  PyObject* consts_;
  PyObject* names_;

  const char* instructions_;

public:
  PyObject* py_call_args;
  ObjVector reg_call_args;
  PyObject** registers;
  PyObject** freevars;
  const RegisterCode* code;

  size_t current_hint;

  f_inline const char* instructions() {
    return instructions_;
  }

  f_inline PyObject* globals() {
    return globals_;
  }
  f_inline PyObject* builtins() {
    return builtins_;
  }

  f_inline PyObject* consts() {
    return consts_;
  }

  f_inline int num_consts() const {
    return PyTuple_GET_SIZE(consts_) ;
  }

  f_inline PyObject* names() {
    return names_;
  }

  void set_local(PyObject* name, PyObject* value) {
    for (int i = 0; i < PyTuple_GET_SIZE(names_) ; ++i) {
      if (PyTuple_GET_ITEM(names_, i) == name) {
        registers[num_consts() + i] = value;
      }
    }
  }

  void fill_locals(PyObject* ldict) {
    for (int i = 0; i < PyTuple_GET_SIZE(names_); ++i) {
      PyObject* name = PyTuple_GET_ITEM(names_, i);
      PyObject* value = PyDict_GetItem(ldict, name);
      if (value != NULL) {
        registers[num_consts() + i] = value;
      }
    }
  }

  f_inline PyObject* locals() {
    if (!locals_) {
      const int num_consts = PyTuple_Size(consts());
      const int num_locals = code->code()->co_nlocals;

      Log_Info("%d %d %d %d", num_consts, num_locals, num_consts + num_locals, code->num_registers);
      locals_ = PyDict_New();
      for (int i = 0; i < num_locals; ++i) {
        PyObject* v = registers[num_consts + i];
        if (v != NULL) {
          Py_INCREF(v);
          PyDict_SetItem(locals_, PyTuple_GetItem(code->names(), i), v);
        }
      }
    }
    return locals_;
  }

  f_inline int offset(const char* pc) const {
    return (int) (pc - instructions_);
  }

  f_inline int next_code(const char* pc) const {
    return ((OpHeader*) pc)->code;
  }

  std::string str() const {
    StringWriter w;
    if (code->function) {
      PyFunctionObject* f = (PyFunctionObject*)code->function;
      w.printf("func: %s ", obj_to_str(f->func_name));
    }
    PyCodeObject* c = (PyCodeObject*)code->code_;
    w.printf("file: %s, line: %d", obj_to_str(c->co_filename), c->co_firstlineno);
    return w.str();
  }

  RegisterFrame(RegisterCode* func, PyObject* obj, const ObjVector& args, const ObjVector& kw);
  ~RegisterFrame();
};

class Evaluator {
private:
  void collect_info(int opcode);
  int32_t op_counts_[256];
  int64_t op_times_[256];

  int32_t total_count_;
  int64_t last_clock_;

  Compiler *compiler_;
public:
  Evaluator();
  ~Evaluator();
  void dump_status();

  inline RegisterCode* compile(PyObject* f);

  PyObject* eval(RegisterFrame* rf);
  PyObject* eval_python(PyObject* func, PyObject* args);

  RegisterFrame* frame_from_pyframe(PyFrameObject*);
  RegisterFrame* frame_from_pyfunc(PyObject* func, PyObject* args, PyObject* kw);
  RegisterFrame* frame_from_codeobj(PyObject* code);

  Hint hints[kMaxHints];
};

RegisterCode* Evaluator::compile(PyObject* obj) {
  return compiler_->compile(obj);
}

//void StartTracing(Evaluator*);
//int TraceFunction(PyObject *obj, PyFrameObject *frame, int what, PyObject *arg);

#endif /* REVAL_H_ */
