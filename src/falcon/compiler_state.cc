#include "compiler_state.h"

void CompilerState::dump(Writer* w) {
  for (BasicBlock* bb : bbs) {
    if (bb->dead) {
      continue;
    }

    w->printf("bb_%d: \n  ", bb->py_offset);
    w->write(StrUtil::join(bb->code, "\n  "));
    w->write(" -> ");
    w->write(StrUtil::join(bb->exits.begin(), bb->exits.end(), ",", [](BasicBlock* n) {
      return StringPrintf("bb_%d", n->py_offset);
    }));
    w->write("\n");
  }
}

std::string CompilerState::str() {
  StringWriter w;
  dump(&w);
  return w.str();
}

BasicBlock* CompilerState::alloc_bb(int offset, RegisterStack* entry_stack) {
  RegisterStack* entry_stack_copy = new RegisterStack(*entry_stack);
  BasicBlock* bb = new BasicBlock(offset, bbs.size(), entry_stack_copy);
  alloc_.push_back(bb);
  bbs.push_back(bb);
  this->bb_offsets[offset] = bb;
  return bb;
}

void CompilerState::remove_bb(BasicBlock* bb) {
  bbs.erase(std::find(bbs.begin(), bbs.end(), bb));
  this->bb_offsets.erase(this->bb_offsets.find(bb->py_offset));
}
