#ifndef FALCON_COMPILER_STATE_H
#define FALCON_COMPILER_STATE_H

#include <vector>
#include <string>
#include <map>

#include "py_include.h"

#include "register_stack.h"
#include "basic_block.h"


struct CompilerState {
private:
  std::vector<BasicBlock*> alloc_;
public:
  std::vector<BasicBlock*> bbs;

  int num_reg;
  int num_consts;
  int num_locals;

  PyCodeObject* py_code;
  PyObject* consts_tuple;
  unsigned char* py_codestr;
  Py_ssize_t py_codelen;
  PyObject* names;


  std::map<int, BasicBlock*> bb_offsets;

  CompilerState() :
      num_reg(0), num_consts(0), num_locals(0),
      py_code(NULL),  consts_tuple(NULL),
      py_codestr(NULL), py_codelen(0),
      names(NULL) { }

  CompilerState(PyCodeObject* code) {

    int codelen = PyString_GET_SIZE(code->co_code);
    py_code = code;
    consts_tuple = code->co_consts;
    num_consts = PyTuple_Size(consts_tuple);
    num_locals = code->co_nlocals;
    // Offset by the number of constants and locals.
    num_reg = num_consts + num_locals;
//    Log_Info("Consts: %d, locals: %d, first register: %d", num_consts, num_locals, num_reg);

    py_codelen = codelen;
    py_codestr = (unsigned char*) PyString_AsString(code->co_code);

    names = code->co_names;

  }

  ~CompilerState() {
    for (auto bb : alloc_) {
      delete bb;
    }
  }

  int num_ops() {
    int total = 0;
    for (auto bb : bbs) {
      total += bb->code.size();
    }
    return total;
  }

  BasicBlock* alloc_bb(int offset, RegisterStack* entry_stack);
  void remove_bb(BasicBlock* bb);
  std::string str();
  void dump(Writer* w);
};

#endif
