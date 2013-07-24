#ifndef OPUTIL_H_
#define OPUTIL_H_

#include "py_include.h"
#include "opcode.h"

#include <set>

#define INCREF 148
#define DECREF 149
#define CONST_INDEX 150
#define BINARY_SUBSCR_LIST 151
#define BINARY_SUBSCR_DICT 152
#define DICT_CONTAINS 153
#define STORE_SUBSCR_LIST 154
#define STORE_SUBSCR_DICT 155

struct OpUtil {
  static const char* name(int opcode);

  static bool has_hint(int opcode) {
    if (opcode == LOAD_ATTR) {
      return true;
    }
    return false;
  }

  static bool is_varargs(int opcode) {
    static std::set<int> r;
    if (r.empty()) {
      r.insert(CALL_FUNCTION);
      r.insert(CALL_FUNCTION_KW);
      r.insert(CALL_FUNCTION_VAR);
      r.insert(CALL_FUNCTION_VAR_KW);
      r.insert(BUILD_LIST);
      r.insert(BUILD_TUPLE);
//      r.insert(BUILD_MAP);
      r.insert(BUILD_SET);
      r.insert(MAKE_FUNCTION);
      r.insert(MAKE_CLOSURE);
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
      r.insert(BREAK_LOOP);
      r.insert(CONTINUE_LOOP);
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
      r.insert(MAKE_FUNCTION);
      r.insert(BUILD_LIST);
      r.insert(BUILD_TUPLE);
      r.insert(BUILD_MAP);
      r.insert(BUILD_SET);
      r.insert(IMPORT_NAME);
      r.insert(IMPORT_FROM);
      r.insert(CONTINUE_LOOP);
    }

    return r.find(opcode) != r.end();
  }
};

#endif /* OPUTIL_H_ */
