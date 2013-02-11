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
#include <boost/noncopyable.hpp>

struct RegisterFrame: private boost::noncopyable {
private:
  PyObject* builtins_;
  PyObject* globals_;

  PyObject* locals_;
  PyObject* args_;
  PyObject* kw_;

public:
  PyObject *call_args;
  PyObject** registers;
  RegisterCode* code;

  const char* instructions;

  f_inline PyObject* globals() {
    return globals_;
  }
  f_inline PyObject* builtins() {
    return builtins_;
  }

  f_inline PyObject* args() {
    return args_;
  }

  f_inline PyObject* kw() {
    return kw_;
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
    return (int) (pc - instructions);
  }

  f_inline int next_code(const char* pc) const {
    return ((RMachineOp*) pc)->header.code;
  }

  RegisterFrame(RegisterCode* func, PyObject* obj, PyObject* args, PyObject* kw);
  ~RegisterFrame();
};

class Evaluator {
private:
  f_inline void collect_info(int opcode);
  int32_t op_counts_[256];
  int64_t op_times_[256];

  int32_t total_count_;
  int64_t last_clock_;

  Compiler *compiler_;
public:

  Evaluator();

  PyObject* eval(RegisterFrame* rf);
  PyObject* eval_python(PyObject* func, PyObject* args);

  RegisterFrame* frame_from_python(PyObject* func, PyObject* args);

  void dump_status();
};

#endif /* REVAL_H_ */
