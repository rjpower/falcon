#ifndef OPUTIL_H_
#define OPUTIL_H_

#include "Python.h"
#include "opcode.h"

#include <set>

#define INCREF 148
#define DECREF 149
#define CONST_INDEX 150

struct OpUtil {
  static const char* name(int opcode);

  static bool is_varargs(int opcode) {
    static std::set<int> r;
    if (r.empty()) {
      r.insert(CALL_FUNCTION);
      r.insert(CALL_FUNCTION_KW);
      r.insert(CALL_FUNCTION_VAR);
      r.insert(CALL_FUNCTION_VAR_KW);
      r.insert(BUILD_LIST);
      r.insert(BUILD_TUPLE);
      r.insert(BUILD_MAP);
      r.insert(BUILD_SET);
    }

    return r.find(opcode) != r.end();
  }

  static bool is_branch(int opcode) {
    static std::set<int> r;
    if (r.empty()) {
      r.insert(FOR_ITER);
      r.insert(JUMP_IF_FALSE_OR_POP);
      r.insert(JUMP_IF_TRUE_OR_POP);
      r.insert(POP_JUMP_IF_FALSE);
      r.insert(POP_JUMP_IF_TRUE);
      r.insert(JUMP_ABSOLUTE);
      r.insert(JUMP_FORWARD);
    }

    return r.find(opcode) != r.end();
  }

  static bool has_arg(int opcode) {
    static std::set<int> r;
    if (r.empty()) {
      r.insert(COMPARE_OP);
      r.insert(LOAD_GLOBAL);
      r.insert(LOAD_NAME);
      r.insert(LOAD_ATTR);
      r.insert(STORE_GLOBAL);
      r.insert(STORE_NAME);
      r.insert(STORE_ATTR);
      r.insert(CONST_INDEX);
      r.insert(CALL_FUNCTION);
      r.insert(CALL_FUNCTION_KW);
      r.insert(CALL_FUNCTION_VAR);
      r.insert(CALL_FUNCTION_VAR_KW);
      r.insert(BUILD_LIST);
      r.insert(BUILD_TUPLE);
      r.insert(BUILD_MAP);
      r.insert(BUILD_SET);
    }

    return r.find(opcode) != r.end();
  }
};

#endif /* OPUTIL_H_ */
