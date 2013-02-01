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

const char* strnstr(const char* haystack, const char* needle, int len) {
  int nlen = strlen(needle);
  for (int i = 0; i < len - nlen; ++i) {
    if (strncmp(haystack + i, needle, nlen) == 0) {
      return haystack + i;
    }
  }
  return NULL;
}

