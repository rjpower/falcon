#include <Python.h>

#include "util.h"
#include <signal.h>
#include <libgen.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

using namespace std;
map<string, double*> TimerRegistry::timers;

LogLevel currentLogLevel = kLogInfo;
static const char* logLevels[5] = { "D", "I", "W", "E", "F" };

void breakpoint() {
  struct sigaction oldAct;
  struct sigaction newAct;
  newAct.sa_handler = SIG_IGN;
  sigaction(SIGTRAP, &newAct, &oldAct);
  raise(SIGTRAP);
  sigaction(SIGTRAP, &oldAct, NULL);
}

const char* opcode_to_name(int opcode) {
  switch (opcode) {
  case 0:
    return "STOP_CODE";
  case 1:
    return "POP_TOP";
  case 2:
    return "ROT_TWO";
  case 3:
    return "ROT_THREE";
  case 4:
    return "DUP_TOP";
  case 5:
    return "ROT_FOUR";
  case 9:
    return "NOP";
  case 10:
    return "UNARY_POSITIVE";
  case 11:
    return "UNARY_NEGATIVE";
  case 12:
    return "UNARY_NOT";
  case 13:
    return "UNARY_CONVERT";
  case 15:
    return "UNARY_INVERT";
  case 19:
    return "BINARY_POWER";
  case 20:
    return "BINARY_MULTIPLY";
  case 21:
    return "BINARY_DIVIDE";
  case 22:
    return "BINARY_MODULO";
  case 23:
    return "BINARY_ADD";
  case 24:
    return "BINARY_SUBTRACT";
  case 25:
    return "BINARY_SUBSCR";
  case 26:
    return "BINARY_FLOOR_DIVIDE";
  case 27:
    return "BINARY_TRUE_DIVIDE";
  case 28:
    return "INPLACE_FLOOR_DIVIDE";
  case 29:
    return "INPLACE_TRUE_DIVIDE";
  case 30:
    return "SLICE";
  case 31:
    return "SLICE";
  case 32:
    return "SLICE";
  case 33:
    return "SLICE";
  case 40:
    return "STORE_SLICE";
  case 41:
    return "STORE_SLICE";
  case 42:
    return "STORE_SLICE";
  case 43:
    return "STORE_SLICE";
  case 50:
    return "DELETE_SLICE";
  case 51:
    return "DELETE_SLICE";
  case 52:
    return "DELETE_SLICE";
  case 53:
    return "DELETE_SLICE";
  case 54:
    return "STORE_MAP";
  case 55:
    return "INPLACE_ADD";
  case 56:
    return "INPLACE_SUBTRACT";
  case 57:
    return "INPLACE_MULTIPLY";
  case 58:
    return "INPLACE_DIVIDE";
  case 59:
    return "INPLACE_MODULO";
  case 60:
    return "STORE_SUBSCR";
  case 61:
    return "DELETE_SUBSCR";
  case 62:
    return "BINARY_LSHIFT";
  case 63:
    return "BINARY_RSHIFT";
  case 64:
    return "BINARY_AND";
  case 65:
    return "BINARY_XOR";
  case 66:
    return "BINARY_OR";
  case 67:
    return "INPLACE_POWER";
  case 68:
    return "GET_ITER";
  case 70:
    return "PRINT_EXPR";
  case 71:
    return "PRINT_ITEM";
  case 72:
    return "PRINT_NEWLINE";
  case 73:
    return "PRINT_ITEM_TO";
  case 74:
    return "PRINT_NEWLINE_TO";
  case 75:
    return "INPLACE_LSHIFT";
  case 76:
    return "INPLACE_RSHIFT";
  case 77:
    return "INPLACE_AND";
  case 78:
    return "INPLACE_XOR";
  case 79:
    return "INPLACE_OR";
  case 80:
    return "BREAK_LOOP";
  case 81:
    return "WITH_CLEANUP";
  case 82:
    return "LOAD_LOCALS";
  case 83:
    return "RETURN_VALUE";
  case 84:
    return "IMPORT_STAR";
  case 85:
    return "EXEC_STMT";
  case 86:
    return "YIELD_VALUE";
  case 87:
    return "POP_BLOCK";
  case 88:
    return "END_FINALLY";
  case 89:
    return "BUILD_CLASS";
  case 90:
    return "STORE_NAME";
  case 91:
    return "DELETE_NAME";
  case 92:
    return "UNPACK_SEQUENCE";
  case 93:
    return "FOR_ITER";
  case 94:
    return "LIST_APPEND";
  case 95:
    return "STORE_ATTR";
  case 96:
    return "DELETE_ATTR";
  case 97:
    return "STORE_GLOBAL";
  case 98:
    return "DELETE_GLOBAL";
  case 99:
    return "DUP_TOPX";
  case 100:
    return "LOAD_CONST";
  case 101:
    return "LOAD_NAME";
  case 102:
    return "BUILD_TUPLE";
  case 103:
    return "BUILD_LIST";
  case 104:
    return "BUILD_SET";
  case 105:
    return "BUILD_MAP";
  case 106:
    return "LOAD_ATTR";
  case 107:
    return "COMPARE_OP";
  case 108:
    return "IMPORT_NAME";
  case 109:
    return "IMPORT_FROM";
  case 110:
    return "JUMP_FORWARD";
  case 111:
    return "JUMP_IF_FALSE_OR_POP";
  case 112:
    return "JUMP_IF_TRUE_OR_POP";
  case 113:
    return "JUMP_ABSOLUTE";
  case 114:
    return "POP_JUMP_IF_FALSE";
  case 115:
    return "POP_JUMP_IF_TRUE";
  case 116:
    return "LOAD_GLOBAL";
  case 119:
    return "CONTINUE_LOOP";
  case 120:
    return "SETUP_LOOP";
  case 121:
    return "SETUP_EXCEPT";
  case 122:
    return "SETUP_FINALLY";
  case 124:
    return "LOAD_FAST";
  case 125:
    return "STORE_FAST";
  case 126:
    return "DELETE_FAST";
  case 130:
    return "RAISE_VARARGS";
  case 131:
    return "CALL_FUNCTION";
  case 132:
    return "MAKE_FUNCTION";
  case 133:
    return "BUILD_SLICE";
  case 134:
    return "MAKE_CLOSURE";
  case 135:
    return "LOAD_CLOSURE";
  case 136:
    return "LOAD_DEREF";
  case 137:
    return "STORE_DEREF";
  case 140:
    return "CALL_FUNCTION_VAR";
  case 141:
    return "CALL_FUNCTION_KW";
  case 142:
    return "CALL_FUNCTION_VAR_KW";
  case 143:
    return "SETUP_WITH";
  case 145:
    return "EXTENDED_ARG";
  case 146:
    return "SET_ADD";
  case 147:
    return "MAP_ADD";
  case 148:
    return "INCREF";
  case 149:
    return "DECREF";
  default:
    return "BAD_OPCODE";
  }
  return "BAD_OPCODE";
}

double clockAsDouble() {
  timespec tp;
  clock_gettime(CLOCK_MONOTONIC, &tp);
  return tp.tv_sec + 1e-9 * tp.tv_nsec;
}

double Now() {
  static double startTime = clockAsDouble();
  return clockAsDouble() - startTime;
}

string Hostname() {
  char hostnameBuffer[1024];
  gethostname(hostnameBuffer, 1024);
  return hostnameBuffer;
}

timeval timevalFromDouble(double t) {
  timeval tv;
  tv.tv_sec = int(t);
  tv.tv_usec = (t - int(t)) * 1e6;
  return tv;
}

timespec timespecFromDouble(double t) {
  timespec tv;
  tv.tv_sec = int(t);
  tv.tv_nsec = (t - int(t)) * 1e9;
  return tv;
}

void Sleep(double time) {
  timespec req = timespecFromDouble(time);
  nanosleep(&req, NULL);
}

double get_processor_frequency() {
  double freq;
  int a, b;
  FILE* procinfo = fopen("/proc/cpuinfo", "r");
  while (fscanf(procinfo, "cpu MHz : %d.%d", &a, &b) != 2) {
    fgetc(procinfo);
  }

  freq = a * 1e6 + b * 1e-4;
  fclose(procinfo);
  return freq;
}

void logAtLevel(LogLevel level, const char* path, int line, const char* fmt, ...) {
  if (level < currentLogLevel) {
    return;
  }

  char buffer[4096];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, 4095, fmt, args);
  va_end(args);

  char file[256];
  memcpy(file, path, 255);
  basename(file);

  double subSecond = Now();

  fprintf(stderr, "%s %4.3f [%5d] %s:%3d %s\n", logLevels[level], subSecond, getpid(), file, line, buffer);

  fflush(stderr);
  if (level == kLogFatal) {
    abort();
  }
}

StringPiece::StringPiece() :
    data_(NULL), len_(0) {
}
StringPiece::StringPiece(const StringPiece& s) :
    data_(s.data_), len_(s.size()) {
}
StringPiece::StringPiece(const string& s) :
    data_(s.data()), len_(s.size()) {
}
StringPiece::StringPiece(const string& s, int len) :
    data_(s.data()), len_(len) {
}
StringPiece::StringPiece(const char* c) :
    data_(c), len_(strlen(c)) {
}
StringPiece::StringPiece(const char* c, int len) :
    data_(c), len_(len) {
}

string StringPiece::AsString() const {
  return string(data_, len_);
}

void StringPiece::strip() {
  while (len_ > 0 && isspace(data_[0])) {
    ++data_;
    --len_;
  }
  while (len_ > 0 && isspace(data_[len_ - 1])) {
    --len_;
  }
}
vector<StringPiece> StringPiece::split(StringPiece sp, StringPiece delim) {
  vector<StringPiece> out;
  const char* c = sp.data_;
  while (c < sp.data_ + sp.len_) {
    const char* next = c;

    bool found = false;

    while (next < sp.data_ + sp.len_) {
      for (int i = 0; i < delim.len_; ++i) {
        if (*next == delim.data_[i]) {
          found = true;
        }
      }
      if (found)
        break;

      ++next;
    }

    if (found || c < sp.data_ + sp.len_) {
      StringPiece part(c, next - c);
      out.push_back(part);
    }

    c = next + 1;
  }

  return out;
}

bool operator==(const StringPiece& a, const StringPiece& b) {
  return a.size() == b.size() && memcmp(a.data(), b.data(), a.size()) == 0;
}

bool operator==(const StringPiece& a, const char* b) {
  bool result = a == StringPiece(b);
  return result;
}

string StringPrintf(StringPiece fmt, ...) {
  va_list l;
  va_start(l, fmt);
  string result = VStringPrintf(fmt, l);
  va_end(l);

  return result;
}

string VStringPrintf(StringPiece fmt, va_list l) {
  char buffer[32768];
  vsnprintf(buffer, 32768, fmt.AsString().c_str(), l);
  return string(buffer);
}

string ToString(int32_t v) {
  return StringPrintf("%d", v);
}

string ToString(int64_t v) {
  return StringPrintf("%ld", v);
}

string ToString(double v) {
  return StringPrintf("%f", v);
}

string ToString(string v) {
  return v;
}
string ToString(StringPiece v) {
  return v.str();
}

const char* strnstr(const char* haystack, const char* needle, int len) {
  int nlen = strlen(needle);
  for (int i = 0; i < len - nlen; ++i) {
    if (strncmp(haystack + i, needle, nlen) == 0) {
      return haystack + i;
    }
  }
  return NULL;
}

