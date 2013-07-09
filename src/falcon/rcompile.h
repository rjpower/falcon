#ifndef RCOMPILE_H_
#define RCOMPILE_H_

#include <cassert>
#include <string>
#include <google/dense_hash_map>

#include "util.h"
#include "rinst.h"
#include "rexcept.h"

// While compiling, we use an expanded form to represent opcodes.  This
// is translated to a compact instruction stream as the last compilation
// step.
struct CompilerOp {
  int code;
  int arg;

  // this instruction has been marked dead by an optimization pass,
  // and should be ignored.
  bool dead;

  // is the last register argument a destination we're writing to?
  bool has_dest;

  std::vector<int> regs;

  std::string str() const;

  CompilerOp(int code, int arg) {
    this->code = code;
    this->arg = arg;
    this->dead = false;
    this->has_dest = false;
  }

  int dest() {
    size_t n_regs = this->regs.size();
    assert(n_regs > 0);
    assert(this->has_dest);
    return this->regs[n_regs - 1];
  }

  size_t num_inputs() {
    size_t n = this->regs.size();
    // if one of the registers is a target for a store, don't count it as an input
    return this->has_dest ? n - 1 : n;
  }
};

struct Frame {
  int target;
  int stack_pos;
};

struct RegisterStack {
  std::vector<int> regs;
  std::vector<Frame> frames;

  RegisterStack() {
  }

  RegisterStack(const RegisterStack& other) {
    this->regs = other.regs;
    this->frames = other.frames;
  }

  void push_frame(int target);
  Frame pop_frame();

  int push_register(int reg);
  int pop_register();
  int peek_register(int reg);

  void fill_register_array(std::vector<int>&, size_t);

  std::string str();
};


struct BasicBlock {
private:
  std::vector<CompilerOp*> alloc_;
  CompilerOp* _add_op(int opcode, int arg, int num_regs);
  CompilerOp* _add_dest_op(int opcode, int arg, int num_regs);
public:

  int py_offset;
  int reg_offset;
  int idx;

  std::vector<BasicBlock*> exits;
  std::vector<BasicBlock*> entries;
  std::vector<CompilerOp*> code;

  // Have we been visited by the current pass already?
  int visited;
  int dead;
  RegisterStack* entry_stack;

  BasicBlock(int offset, int idx, RegisterStack* entry_stack);
  ~BasicBlock() {
    for (auto op : alloc_) {
      delete op;
    }
    delete entry_stack;
  }

  /* operations without a destination register */
  CompilerOp* add_op(int opcode, int arg);
  CompilerOp* add_op(int opcode, int arg, int reg1);
  CompilerOp* add_op(int opcode, int arg, int reg1, int reg2);
  CompilerOp* add_op(int opcode, int arg, int reg1, int reg2, int reg3);
  CompilerOp* add_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4);

  /* operations with a destination register */
  CompilerOp* add_dest_op(int opcode, int arg);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1, int reg2);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4);
  CompilerOp* add_dest_op(int opcode, int arg, int reg1, int reg2, int reg3, int reg4, int reg5);

  CompilerOp* add_varargs_op(int opcode, int arg, int num_regs);
};


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

  CodeCache::iterator i = cache_.find(func);

  if (i != cache_.end()) {
    return i->second;
  }


  try {
    RegisterCode* code = compile_(func);
    cache_[func] = code;
  } catch (RException& e) {
    cache_[func] = NULL;
    throw e;
  }

  return cache_[func];
}

#endif /* RCOMPILE_H_ */
