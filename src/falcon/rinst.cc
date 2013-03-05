#include "rinst.h"

template<int num_registers>
std::string RegOp<num_registers>::str(Register* registers) const {
  StringWriter w;
  w.printf("%s.%d (", OpUtil::name(code), arg);
  for (int i = 0; i < num_registers; ++i) {
//    if (registers == NULL || reg[i] == kInvalidRegister) {
      w.printf("[%d],", reg[i]);
//    } else {
//      w.printf("[%d] %.20s, ", reg[i], obj_to_str(registers[reg[i]].as_obj()));
//    }
  }
  w.printf(")");
  return w.str();
}

std::string VarRegOp::str(Register* registers) const {
  StringWriter w;
  w.printf("%s.%d (", OpUtil::name(code), arg);
  for (int i = 0; i < num_registers; ++i) {
//    if (registers == NULL || reg[i] == kInvalidRegister) {
      w.printf("[%d],", reg[i]);
//    } else {
//      w.printf("[%d] %.20s, ", reg[i], obj_to_str(registers[reg[i]].as_obj()));
//    }
  }
  w.printf(")");
  return w.str();
}

std::string BranchOp::str() const {
  StringWriter w;
  w.printf("%s (", OpUtil::name(code));
  if (reg[0] != kInvalidRegister) {
    w.printf("%d, ", reg[0]);
  }
  if (reg[1] != kInvalidRegister) {
    w.printf("%d, ", reg[1]);
  }
  w.printf(")");
  w.printf(" -> [%d]", label);
  return w.str();
}

const char* obj_to_str(PyObject* o) {
  if (o == NULL) {
    return "<NULL>";
  }
  if (PyString_Check(o)) {
    return PyString_AsString(o);
  }

  PyObject* obj_repr = PyObject_Repr(o);
  if (obj_repr == NULL) {
    return "<INVALID __repr__>";
  }
  return PyString_AsString(obj_repr);
}

template class RegOp<0> ;
template class RegOp<1> ;
template class RegOp<2> ;
template class RegOp<3> ;
template class RegOp<4> ;
