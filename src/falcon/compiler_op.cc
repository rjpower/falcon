#include "compiler_op.h"
#include "oputil.h"
std::string CompilerOp::str() const {
  if (code == LOAD_FAST || code == STORE_FAST) {
    return StringPrintf("r%d = r%d", regs[1], regs[0]);
  }

  StringWriter w;
  int num_args = regs.size();
  if (has_dest) {
    --num_args;
    w.printf("r%d = ", regs[regs.size() - 1]);
  }

  w.printf("%s", OpUtil::name(code));
  if (OpUtil::has_arg(code)) {
    w.printf("[%d]", arg);
  }
  w.printf("(");

  for (int i = 0; i < num_args; ++i) {
    w.printf("r%d", regs[i]);
    if (i < num_args - 1) {
      w.printf(", ");
    }
  }
  w.printf(")");

  return w.str();
}
