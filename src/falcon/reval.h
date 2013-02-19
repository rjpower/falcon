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

typedef std::vector<PyObject*> ObjVector;

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

  f_inline PyObject* names() {
    return names_;
  }

  f_inline PyObject* locals() {
    if (!locals_) {
      const int num_consts = PyTuple_Size(consts());
      const int num_locals = PyTuple_Size(code->names());
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

  RegisterFrame(RegisterCode* func, PyObject* obj, const ObjVector* args, const ObjVector* kw);
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

  RegisterFrame* frame_from_python(PyObject* func, PyObject* args);
  RegisterFrame* frame_from_codeobj(PyObject* code);

  Hint hints[kMaxHints];
};

RegisterCode* Evaluator::compile(PyObject* obj) {
  return compiler_->compile(obj);
}

#endif /* REVAL_H_ */
