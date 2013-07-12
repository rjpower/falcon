#ifndef FALCON_COMPILER_PASS_H
#define FALCON_COMPILER_PASS_H

#include <queue>


#include "compiler_op.h"
#include "basic_block.h"

class CompilerPass {
protected:
public:
  virtual void visit_op(CompilerOp* op) {
  }

public:
  virtual void visit_bb(BasicBlock* bb) {
    size_t n_ops = bb->code.size();
    for (size_t i = 0; i < n_ops; ++i) {
      CompilerOp* op = bb->code[i];
      if (!op->dead) {
        this->visit_op(op);
      }
    }
  }

  virtual void visit_fn(CompilerState* fn) {
    size_t n_bbs = fn->bbs.size();
    for (size_t i = 0; i < n_bbs; ++i) {
      fn->bbs[i]->visited = false;
    }

    for (size_t i = 0; i < n_bbs; ++i) {
      BasicBlock* bb = fn->bbs[i];
      if (!bb->visited && !bb->dead) {
        this->visit_bb(bb);
        bb->visited = true;
      }
    }
  }

  void operator()(CompilerState* fn) {
    this->visit_fn(fn);
  }

  virtual ~CompilerPass() {
  }

};

class BackwardPass: public CompilerPass {
public:
  void visit_bb(BasicBlock* bb) {
    size_t n_ops = bb->code.size();
    for (size_t i = n_ops - 1; i-- > 0;) {
      CompilerOp* op = bb->code[i];
      if (!op->dead) {
        this->visit_op(op);
      }
    }
  }

  void visit_fn(CompilerState* fn) {
    size_t n_bbs = fn->bbs.size();
    for (size_t i = 0; i < n_bbs; ++i) {
      fn->bbs[i]->visited = false;
    }

    for (size_t i = n_bbs - 1; i-- > 0;) {
      BasicBlock* bb = fn->bbs[i];
      if (!bb->visited && !bb->dead) {
        this->visit_bb(bb);
        bb->visited = true;
      }
    }
  }

};

class SortedPass: public CompilerPass {
// visit basic blocks in topologically sorted order
private:
  bool all_preds_visited(BasicBlock* bb) {
    size_t n_entries = bb->entries.size();
    for (size_t i = 0; i < n_entries; ++i) {
      if (!bb->entries[i]->visited) {
        return false;
      }
    }
    return true;
  }
protected:
  bool in_cycle;
public:

  void visit_fn(CompilerState* fn) {
    size_t n_bbs = fn->bbs.size();
    for (size_t i = 0; i < n_bbs; ++i) {
      fn->bbs[i]->visited = false;
    }

    std::queue<BasicBlock*> ready;
    std::queue<BasicBlock*> waiting;

    ready.push(fn->bbs[0]);

    while (ready.size() > 0 || waiting.size() > 0) {
      BasicBlock* bb = NULL;
      if (ready.size() > 0) {
        bb = ready.front();
        ready.pop();
      } else {
        bb = waiting.front();
        waiting.pop();
      }
      if (!bb->visited) {
        bb->visited = true;
        this->in_cycle = !(this->all_preds_visited(bb));
        this->visit_bb(bb);
        int n_exits = bb->exits.size();
        for (int i = 0; i < n_exits; ++i) {
          BasicBlock* succ = bb->exits[i];
          if (this->all_preds_visited(succ)) {
            ready.push(succ);
          } else {
            waiting.push(succ);
          }
        }
      }
    }
  }

};

#endif
