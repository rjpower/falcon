#include "rexcept.h"

#include "register_stack.h"

int RegisterStack::num_exc_handlers() {
  return exc_handlers.size();
}

void RegisterStack::push_exc_handler(int target) {
  Frame f;
  f.stack_pos = regs.size();
  f.target = target;
  exc_handlers.push_back(f);
}

Frame RegisterStack::pop_exc_handler() {
  Frame f = exc_handlers.back();
  exc_handlers.pop_back();
  regs.resize(f.stack_pos);
  return f;
}

void RegisterStack::push_frame(int target) {
  Frame f;
  f.stack_pos = regs.size();
  f.target = target;
  frames.push_back(f);
}

Frame RegisterStack::pop_frame() {
  Frame f = frames.back();
  frames.pop_back();
  regs.resize(f.stack_pos);
  return f;
}

int RegisterStack::push_register(int reg) {
  // Log_Info("Pushing register %d, pos %d", reg, stack_pos + 1);
  regs.push_back(reg);
  return reg;
}

int RegisterStack::pop_register() {
  Reg_AssertGt((int)regs.size(), 0);
  int reg = regs.back();
  regs.pop_back();
  return reg;
}

void RegisterStack::fill_register_array(std::vector<int>& other_regs, size_t n) {
  /* pop registers in reverse order */
  for (int r = n - 1; r >= 0; --r) {
    other_regs[r] = this->pop_register();
  }
}

// Implement ceval's slightly odd PEEK semantics.  To wit: offset
// zero is invalid, the offset of the top register on the stack is
// 1.
int RegisterStack::peek_register(int offset) {
  Reg_AssertGt(offset, 0);
  Reg_AssertGe((int)regs.size(), offset);
  int val = regs[regs.size() - offset];
//  Log_Info("Peek: %d = %d", offset, val);
  return val;
}

std::string RegisterStack::str() {
  return StringPrintf("[%s]", StrUtil::join(regs.begin(), regs.end(), ",", &Coerce::t_str<int>).c_str());
}
