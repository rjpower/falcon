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

// Only enable exception support in debug mode for now.




struct Compiler {
private:
  typedef google::dense_hash_map<PyObject*, RegisterCode*> CodeCache;
  CodeCache cache_;
  BasicBlock* registerize(CompilerState* state, RegisterStack *stack, int offset);
  RegisterCode* compile_(PyObject* function);
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
  Py_IncRef(func);


//  Log_Info("Checking cache for %p...", func);
  CodeCache::iterator i = cache_.find(func);
  if (i != cache_.end()) {
//    Log_Info("Hit.");
    return i->second;
  }
//  Log_Info("Miss.");


  try {
    RegisterCode* code = compile_(func);
    cache_[func] = code;
  } catch (RException& e) {
    Py_DecRef(func);
    Log_Info("Failed to compile function %s", PyEval_GetFuncName(func));
    cache_[func] = NULL;
    throw e;
  }

  return cache_[func];
}

#endif /* RCOMPILE_H_ */
