#include "rinst.h"

static bool objstr_enabled() {
  static bool _enabled = getenv("WITH_OBJSTR") != NULL;
  return _enabled;
}

static void print_register(Writer& w, Register* registers, int reg_num) {
  if (reg_num >= kInvalidRegister) {
    w.printf("NULL,");
  } else if (registers == NULL) {
    w.printf("[%d],", reg_num);
  } else {
    if (objstr_enabled()) {
      w.printf("[%d] %.20s ", reg_num, obj_to_str(registers[reg_num].as_obj()));
    } else {
      w.printf("[%d] %p, ", reg_num, registers[reg_num]);
    }
  }
}

template<int num_registers>
std::string RegOp<num_registers>::str(Register* registers) const {
  StringWriter w;
  w.printf("%s.%d (", OpUtil::name(code), arg);
  for (int i = 0; i < num_registers; ++i) {
    print_register(w, registers, reg[i]);
  }
  w.printf(")");
  return w.str();
}

std::string VarRegOp::str(Register* registers) const {
  StringWriter w;
  w.printf("%s.%d (", OpUtil::name(code), arg);
  for (int i = 0; i < num_registers; ++i) {
    print_register(w, registers, reg[i]);
  }
  w.printf(")");
  return w.str();
}

template<int num_registers>
std::string BranchOp<num_registers>::str(Register* registers) const {
  StringWriter w;
  w.printf("%s (", OpUtil::name(code));
  for (int i = 0; i < num_registers; ++i) {
    print_register(w, registers, reg[i]);
  }
  w.printf(")");
  w.printf(" -> [%d]", label);
  return w.str();
}

template class RegOp<0> ;
template class RegOp<1> ;
template class RegOp<2> ;
template class RegOp<3> ;
template class RegOp<4> ;


template class BranchOp<0> ;
template class BranchOp<1> ;
template class BranchOp<2> ;
