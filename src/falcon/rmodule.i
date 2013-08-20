%module falcon_core


%{
#include "rinst.h"
#include "rcompile.h"
#include "reval.h"
%}

%exception {
  try {
    $function
  } catch (RException& e) {
    PyErr_SetObject(e.exception, e.value);
    return NULL;
  }
}

class Evaluator {
public:
  Evaluator();
  ~Evaluator();
  PyObject* eval_python(PyObject* func, PyObject* args, PyObject* kw);
  PyObject* eval_python_module(PyObject* code, PyObject* module_dict);
};
