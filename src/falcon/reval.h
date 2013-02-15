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
    return code->consts();
  }

  f_inline PyObject* names() {
    return code->names();
  }

  f_inline PyObject* locals() {
    PyObject* consts = code->consts();
    int num_consts = PyTuple_Size(consts);
    int num_locals = PyTuple_Size(code->names());
    if (!locals_) {
      locals_ = PyDict_New();
      for (int i = 0; i < num_locals; ++i) {
        PyDict_SetItem(locals_, PyTuple_GetItem(code->names(), i), registers[num_consts + i]);
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

  RegisterCode* compile(PyObject* f);

  PyObject* eval(RegisterFrame* rf);
  PyObject* eval_python(PyObject* func, PyObject* args);
  RegisterFrame* frame_from_python(PyObject* func, PyObject* args);

  Hint hints[kMaxHints];
};

#endif /* REVAL_H_ */
