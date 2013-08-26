#ifndef RCOMPILE_H_
#define RCOMPILE_H_

#include <cassert>
#include <string>
#include <map>
#include <vector>

#include <google/dense_hash_map>

#include "util.h"
#include "rinst.h"
#include "rexcept.h"

#include "compiler_op.h"
#include "basic_block.h"
#include "register_stack.h"
#include "compiler_state.h"


struct Compiler {
private:
  typedef google::dense_hash_map<PyObject*, RegisterCode*> CodeCache;
  CodeCache cache_;
  BasicBlock* registerize(CompilerState* state, RegisterStack *stack, int offset);
  RegisterCode* compile_(PyObject* function);


  const char* fn_name(PyObject* func) {
    if (PyFunction_Check(func)) {
      return PyEval_GetFuncName(func);
    } else {
      return obj_to_str(func);
    }
  }

public:
  Compiler() {
    cache_.set_empty_key(NULL);
  }

  inline RegisterCode* compile(PyObject* function);
};



RegisterCode* Compiler::compile(PyObject* func) {


  if (PyMethod_Check(func)) {

    func = PyMethod_GET_FUNCTION(func);
  }

  PyObject* stack_code = NULL;

  if (PyFunction_Check(func)) {

    stack_code = PyFunction_GET_CODE(func);
  } else if (PyCode_Check(func)) {
    stack_code = func;
  }

  if (stack_code == NULL) {
    printf("NO STACK CODE\n");
    if (PyObject_HasAttrString(func, "__call__")) {
      printf("HASATTR\n");
      PyObject* call_method = PyObject_GetAttrString(func, "__call__");
      return compile(call_method);
    }

    Log_Info("No code for function %s", fn_name(func));
    return NULL;
  }

  auto iter = cache_.find(stack_code);
  if (iter != cache_.end()) { return iter->second; }

  RegisterCode* register_code = NULL;
  try {
     register_code = compile_(func);
  } catch (const RException& e) {
    Log_Info("Failed to compile function %s", fn_name(func));
  }
  cache_[stack_code] = register_code;

  return register_code;

}

#endif /* RCOMPILE_H_ */
