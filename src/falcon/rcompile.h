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
  /*
   * Expects func to either be a function or a code object.
   * All other callables (methods, old-style classes, type objects, and user-defined callables)
   * have to be disassembled into their underlying function before you call 'compile'.
   */


  /*if (PyMethod_Check(func)) {

    func = PyMethod_GET_FUNCTION(func);
  }
  */
  PyObject* stack_code = NULL;

  if (PyFunction_Check(func)) {
    stack_code = PyFunction_GET_CODE(func);
  } else {
    Reg_Assert(PyCode_Check(func),
               "compile expects either function or code, got %s",
               obj_to_str(func));
    stack_code = func;
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
