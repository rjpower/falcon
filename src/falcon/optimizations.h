#ifndef FALCON_OPTIMIZATIONS_H
#define FALCON_OPTIMIZATIONS_H

#include <map>

#include "opcode.h"
#include "util.h"
#include "compiler_pass.h"
#include "basic_block.h"

#define COMPILE_LOG(...) do { if (getenv("COMPILE_LOG")) { Log_Info(__VA_ARGS__); } } while (0)

class UseCounts {
protected:
  std::map<int, int> counts;

  int get_count(int r) {
    std::map<int, int>::iterator iter = counts.find(r);
    return iter == counts.end() ? 0 : iter->second;
  }

  void incr_count(int r) {
    this->counts[r] = this->get_count(r) + 1;
  }

  void decr_count(int r) {
    this->counts[r] = this->get_count(r) - 1;
  }

  bool is_pure(int op_code) {
    Log_Debug("Checking if %s is pure\n", OpUtil::name(op_code));
    switch (op_code) {
    case LOAD_GLOBAL:
    case LOAD_FAST:
    case LOAD_DEREF:
    case LOAD_CLOSURE:
    case LOAD_LOCALS:
    case LOAD_CONST:
    case LOAD_NAME:
    case STORE_FAST:
    case STORE_DEREF:
    case BUILD_SLICE:
    case CONST_INDEX:
    case BUILD_TUPLE:
    case BUILD_LIST:
    case BUILD_SET:
    case BUILD_MAP:
    case MAKE_CLOSURE:
      return true;
    default:
      return false;
    }
  }
  void count_uses(CompilerState* fn) {
    size_t n_bbs = fn->bbs.size();
    for (size_t bb_idx = 0; bb_idx < n_bbs; ++bb_idx) {

      BasicBlock* bb = fn->bbs[bb_idx];
      size_t n_ops = bb->code.size();
      for (size_t op_idx = 0; op_idx < n_ops; ++op_idx) {
        CompilerOp* op = bb->code[op_idx];
        size_t n_inputs = op->num_inputs();

        if (n_inputs > 0) {
          for (size_t reg_idx = 0; reg_idx < n_inputs; reg_idx++) {
            this->incr_count(op->regs[reg_idx]);
          }
        }
      }
    }
  }
};


class MarkEntries: public CompilerPass {
public:
  void visit_bb(BasicBlock* bb) {
    for (size_t i = 0; i < bb->exits.size(); ++i) {
      BasicBlock* next = bb->exits[i];
      next->entries.push_back(bb);
    }
  }
};

class FuseBasicBlocks: public CompilerPass {
public:
  void visit_bb(BasicBlock* bb) {
    if (bb->visited || bb->dead || bb->exits.size() != 1) {
      return;
    }

    BasicBlock* next = bb->exits[0];
    while (1) {
      if (next->entries.size() > 1 || next->visited) {
        break;
      }

      // Strip our branch instruction if we're being merged
      // into the following basic block.
      if (!bb->code.empty()) {
        CompilerOp* last = bb->code.back();
        if (OpUtil::is_branch(last->code)) {
          bb->code.pop_back();
        }
      }

      //        Log_Info("Merging %d into %d", next->idx, bb->idx);
      bb->code.insert(bb->code.end(), next->code.begin(), next->code.end());

      next->dead = next->visited = true;
      next->code.clear();
      bb->exits = next->exits;

      if (bb->exits.size() != 1) {
        break;
      }
      next = bb->exits[0];
    }

  }
};

class CopyPropagation: public CompilerPass {
public:
  void visit_bb(BasicBlock* bb) {
    std::map<int, int> env;
    size_t n_ops = bb->code.size();
    int source, target;
    for (size_t i = 0; i < n_ops; ++i) {
      CompilerOp * op = bb->code[i];

      // check all the registers and forward any that are in the env
      size_t n_inputs = op->num_inputs();
      for (size_t reg_idx = 0; reg_idx < n_inputs; reg_idx++) {
        auto iter = env.find(op->regs[reg_idx]);
        if (iter != env.end()) {
          op->regs[reg_idx] = iter->second;
        }
      }
      if (op->code == LOAD_FAST || op->code == STORE_FAST || op->code == LOAD_CONST) {
        source = op->regs[0];
        target = op->regs[1];
        auto iter = env.find(source);
        if (iter != env.end()) {
          source = iter->second;
        }
        env[target] = source;
      }
    }
  }
};

class StoreElim: public CompilerPass, UseCounts {
public:
  void visit_bb(BasicBlock* bb) {
    // map from registers to their last definition in the basic block
    std::map<int, CompilerOp*> env;

    // if we encounter a move X->Y when:
    //   - X is locally defined in the basic block
    //   - X is only used once (for this move)
    // then modify the defining instruction of X
    // to directly write to Y and mark the move X->Y as dead

    size_t n_ops = bb->code.size();
    int source, target;
    for (size_t i = 0; i < n_ops; ++i) {
      CompilerOp * op = bb->code[i];
      // check all the registers and forward any that are in the env
      size_t n_inputs = op->num_inputs();

      if (op->has_dest) {
        target = op->regs[n_inputs];
        env[target] = op;

        if (op->code == LOAD_FAST || op->code == STORE_FAST) {
          source = op->regs[0];
          auto iter = env.find(source);
          if (iter != env.end() && this->get_count(source) == 1) {
            CompilerOp* def = iter->second;
            def->regs[def->num_inputs()] = target;
            op->dead = true;
          }
        }
      }
    }
  }

  void visit_fn(CompilerState* fn) {
    this->count_uses(fn);
    CompilerPass::visit_fn(fn);
  }
};

class DeadCodeElim: public BackwardPass, UseCounts {
private:
public:
  void remove_dead_ops(BasicBlock* bb) {
    size_t live_pos = 0;
    size_t n_ops = bb->code.size();
    for (size_t i = 0; i < n_ops; ++i) {
      CompilerOp* op = bb->code[i];
      if (op->dead) {
        continue;
      }
      bb->code[live_pos++] = op;
    }

    bb->code.resize(live_pos);
  }

  void remove_dead_code(CompilerState* fn) {
    size_t i = 0;
    size_t live_pos = 0;
    size_t n_bbs = fn->bbs.size();
    for (i = 0; i < n_bbs; ++i) {
      BasicBlock* bb = fn->bbs[i];
      if (bb->dead) {
        continue;
      }

      this->remove_dead_ops(bb);
      fn->bbs[live_pos++] = bb;
    }
    fn->bbs.resize(live_pos);
  }

  void visit_op(CompilerOp* op) {

    size_t n_inputs = op->num_inputs();
    if ((n_inputs > 0) && (op->has_dest)) {
      int dest = op->regs[n_inputs];
      if (this->is_pure(op->code) && this->get_count(dest) == 0) {
        op->dead = true;
        // if an operation is marked dead, decrement the use counts
        // on all of its arguments
        for (size_t input_idx = 0; input_idx < n_inputs; ++input_idx) {
          this->decr_count(op->regs[input_idx]);
        }
      }
    }
  }

  void visit_fn(CompilerState* fn) {
    this->count_uses(fn);
    BackwardPass::visit_fn(fn);
    remove_dead_code(fn);
  }
};

class RenameRegisters: public CompilerPass {
  // simple renaming that ignore live ranges of registers
private:
  // Mapping from old -> new register names
  std::map<int, int> register_map_;

public:
  RenameRegisters() {
  }

  void visit_op(CompilerOp* op) {
    for (size_t i = 0; i < op->regs.size(); ++i) {
      int tgt;
      if (register_map_.find(op->regs[i]) == register_map_.end()) {
        Log_Fatal("No mapping for register: %s, [%d]", op->str().c_str(), op->regs[i]);
      }
      tgt = register_map_[op->regs[i]];
      op->regs[i] = tgt;
    }
  }

  void visit_fn(CompilerState* fn) {

    std::map<int, int> counts;

    for (BasicBlock* bb : fn->bbs) {
      if (bb->dead) continue;
      for (CompilerOp *op : bb->code) {
        if (op->dead) continue;
        for (int reg : op->regs) {
          ++counts[reg];
        }
      }
    }

    // A few fixed-register opcodes special case the invalid register.
    register_map_[-1] = -1;

    // Don't remap the const/local register aliases, even if we
    // don't see a usage point for them.
    for (int i = 0; i < fn->num_consts + fn->num_locals; ++i) {
      register_map_[i] = i;
    }

    int curr = fn->num_consts + fn->num_locals;
    for (int i = fn->num_consts + fn->num_locals; i < fn->num_reg; ++i) {
      if (counts[i] != 0) {
        register_map_[i] = curr++;
      }
    }

    int min_count = 0;
    for (int i = 0; i < fn->num_reg; ++i) {
      if (counts[i] != 0) {
        ++min_count;
      }
    }

    CompilerPass::visit_fn(fn);
    COMPILE_LOG(
        "Register rename: keeping %d of %d registers (%d const+local, with arg+const folding: %d)", curr, fn->num_reg, fn->num_consts + fn->num_locals, min_count);
    fn->num_reg = curr;
  }
};

class CompactRegisters: public SortedPass, UseCounts {
private:
  // Mapping from old -> new register names
  std::map<int, int> register_map;
  std::stack<int> free_registers;
  int num_frozen;
  int max_register;

  std::set<int> bb_defs;
  bool defined_locally(int r) {
    return bb_defs.find(r) != bb_defs.end();
  }

public:

  void visit_op(CompilerOp* op) {

    size_t n_regs = op->regs.size();
    size_t n_input_regs;
    if (op->has_dest) {
      n_input_regs = n_regs - 1;
    } else {
      n_input_regs = n_regs;
    }
    int old_reg;
    int new_reg;
    for (size_t i = 0; i < n_input_regs; ++i) {
      old_reg = op->regs[i];
      if (old_reg >= num_frozen) {
        new_reg = register_map[old_reg];
        if (new_reg != 0) {
          op->regs[i] = new_reg;
          this->decr_count(old_reg);
          if (this->get_count(old_reg) == 0) {
            if (!this->in_cycle || this->defined_locally(old_reg)) {
              this->free_registers.push(new_reg);
            }
          }
        }
      }
    }
    if (op->has_dest) {
      old_reg = op->regs[n_input_regs];
      if (old_reg >= 0) {
        bb_defs.insert(old_reg);
        if (register_map.find(old_reg) != register_map.end()) {
          new_reg = register_map[old_reg];
        } else if (free_registers.size() > 0) {
          new_reg = free_registers.top();
          free_registers.pop();
          register_map[old_reg] = new_reg;
        } else {
          new_reg = max_register;
          register_map[old_reg] = new_reg;
          max_register++;
        }
        op->regs[n_input_regs] = new_reg;
      }
    }
  }

  void visit_bb(BasicBlock* bb) {
    this->bb_defs.clear();
    SortedPass::visit_bb(bb);

  }
  void visit_fn(CompilerState* fn) {

    this->count_uses(fn);

    // don't rename inputs, locals, or constants
    num_frozen = fn->num_locals + fn->num_consts;
    max_register = num_frozen;
    for (int i = 0; i < max_register; ++i) {
      register_map[i] = i;
    }
    SortedPass::visit_fn(fn);
  }
};



enum StaticType {
  UNKNOWN,
  INT,
  FLOAT,
  LIST,
  TUPLE,
  DICT,
  OBJ
};




class TypeInference {
protected:
  std::map<int, StaticType> types;

  StaticType get_type(int r)  {
    std::map<int, StaticType>::iterator iter = this->types.find(r);
    if (iter == this->types.end()) {
      return UNKNOWN;
    } else {
      return iter->second;
    }
  }

  void update_type(int r, StaticType t) {
    StaticType old_t = this->get_type(r);

    if (old_t == UNKNOWN) {
      this->types[r] = t;
    } else if (old_t != t && old_t != OBJ) {
      this->types[r] = OBJ;
    }
  }

public:
  void infer(CompilerState* fn) {

    // initialize the constants to their types
    for (int i = 0; i < fn->num_consts; ++i) {
      PyObject* obj = PyTuple_GetItem(fn->consts_tuple, i);

      if (PyInt_CheckExact(obj)) {
        this->update_type(i, INT);
      } else if (PyFloat_CheckExact(obj)) {
        this->update_type(i, FLOAT);
      } else {
        this->update_type(i, OBJ);
      }
    }
    size_t n_bbs = fn->bbs.size();
    for (size_t bb_idx = 0; bb_idx < n_bbs; ++bb_idx) {

      BasicBlock* bb = fn->bbs[bb_idx];
      size_t n_ops = bb->code.size();
      for (size_t op_idx = 0; op_idx < n_ops; ++op_idx) {

        CompilerOp* op = bb->code[op_idx];

        size_t n_inputs = op->num_inputs();
        if (op->has_dest) {
          int dest = op->regs[n_inputs];
          StaticType t = OBJ;
          switch (op->code) {
          case BUILD_LIST:
            t = LIST;
            break;
          case BUILD_TUPLE:
            t = TUPLE;
            break;
          case BUILD_MAP:
            t = DICT;
            break;
          case LOAD_FAST: {
            int src_reg = op->regs[0];
            if (src_reg < fn->num_consts) {
              PyObject* const_obj = PyTuple_GetItem(fn->consts_tuple, src_reg);

              if (PyInt_CheckExact(const_obj)) {
                t = INT;
              } else if (PyFloat_CheckExact(const_obj)) {
                t = FLOAT;
              } else {
                t = OBJ;
              }
            } else {
              t = OBJ;
            }
          }
            break;
          }
          this->update_type(dest, t);
        }
      }
    }
  }
};



enum KnownMethod {
  METHOD_UNKNOWN,
  METHOD_LIST_APPEND
};

class LocalTypeSpecialization: public CompilerPass, protected TypeInference {
private:
  std::map<int, KnownMethod> known_methods;
  std::map<int, int> known_bound_objects;

  KnownMethod find_method(int reg) {
    auto iter = this->known_methods.find(reg);
    if (iter == this->known_methods.end()) {
      return METHOD_UNKNOWN;
    } else {
      return iter->second;
    }
  }

  PyObject* names;
  PyObject* consts_tuple;

public:
  void visit_op(CompilerOp* op) {
    switch (op->code) {
    case LOAD_ATTR: {
      PyObject* attr_name_obj = PyTuple_GetItem(this->names, op->arg);

      char* attr_name = PyString_AsString(attr_name_obj);

      if (strcmp(attr_name, "append") == 0) {
        this->known_methods[op->regs[1]] = METHOD_LIST_APPEND;
        this->known_bound_objects[op->regs[1]] = op->regs[0];
      }
      break;
    }

    case CALL_FUNCTION: {
      int fn_reg = op->regs[0];
      if (this->find_method(fn_reg) == METHOD_LIST_APPEND) {
        op->code = LIST_APPEND;
        int item = op->regs[1];
        op->has_dest = false;
        op->arg = 0;
        op->regs.clear();

        op->regs.push_back(this->known_bound_objects[fn_reg]);
        op->regs.push_back(item);
      }
      break;
    }
    case BINARY_SUBSCR: {
      StaticType t = this->get_type(op->regs[0]);
      if (t == LIST) {
        op->code = BINARY_SUBSCR_LIST;
      } else if (t == DICT) {
        op->code = BINARY_SUBSCR_DICT;
      }
      break;
    }
    case STORE_SUBSCR: {
      StaticType t = this->get_type(op->regs[1]);
      if (t == LIST) {
        op->code = STORE_SUBSCR_LIST;
      } else if (t == DICT) {
        op->code = STORE_SUBSCR_DICT;
      }
      break;
    }
    case COMPARE_OP: {
      // specialize '__contains__'
      if (op->arg == 6 && this->get_type(op->regs[0]) == DICT) {
        op->code = DICT_CONTAINS;
      }
      break;
    }
    }
  }

  void visit_fn(CompilerState* fn) {
    this->infer(fn);
    this->names = fn->names;
    this->consts_tuple = fn->consts_tuple;
    CompilerPass::visit_fn(fn);

  }
};

void optimize(CompilerState* fn) {
  MarkEntries()(fn);
  FuseBasicBlocks()(fn);

  if (!getenv("DISABLE_OPT")) {
    if (!getenv("DISABLE_COPY")) CopyPropagation()(fn);
    if (!getenv("DISABLE_STORE")) StoreElim()(fn);
  }
  COMPILE_LOG(fn->str().c_str());
  DeadCodeElim()(fn);

  if (!getenv("DISABLE_OPT")) {
    if (!getenv("DISABLE_SPECIALIZATION")) LocalTypeSpecialization()(fn);
  }

  DeadCodeElim()(fn);
  if (!getenv("DISABLE_OPT")) {
    if (!getenv("DISABLE_COMPACT")) CompactRegisters()(fn);
  }

  RenameRegisters()(fn);
  COMPILE_LOG(fn->str().c_str());
}


#endif
