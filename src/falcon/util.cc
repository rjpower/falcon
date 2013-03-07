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
map<string, double*> Counters::counters;

LogLevel currentLogLevel = kLogInfo;
static const char* logLevels[5] = { "D", "I", "W", "E", "F" };

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
  static const int buffer_size = 100000;
  static char buffer[buffer_size];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, buffer_size - 1, fmt, args);
  va_end(args);

  char file[256];
  memcpy(file, path, 255);
  basename(file);

  double subSecond = Now();

  fprintf(stderr, "%s %4.3f [%5d] %s:%d %s\n", logLevels[level], subSecond, getpid(), file, line, buffer);

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

vector<StringPiece> StrUtil::split(StringPiece sp, StringPiece delim) {
  vector<StringPiece> out;
  const char* c = sp.data();
  while (c < sp.data() + sp.size()) {
    const char* next = c;

    bool found = false;

    while (next < sp.data() + sp.size()) {
      for (int i = 0; i < delim.size(); ++i) {
        if (*next == delim.data()[i]) {
          found = true;
        }
      }
      if (found) break;

      ++next;
    }

    if (found || c < sp.data() + sp.size()) {
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

std::string Coerce::str(const std::string& v) {
  return v;
}

std::string Coerce::str(const double& v) {
  return StringPrintf("%f", v);
}

std::string Coerce::str(const int& v) {
  return StringPrintf("%d", v);
}

std::string Coerce::str(const short & v) {
  return StringPrintf("%d", v);
}

void Writer::printf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::string res = VStringPrintf(fmt, args);
  va_end(args);
  this->write(res);
}

void Writer::write(const char* str) {
  write(std::string(str));
}

void StringWriter::write(const std::string& data) {
  buffer_.append(data);
}

const std::string& StringWriter::str() {
  return buffer_;
}

void FileWriter::write(const std::string& data) {
  int result = ::write(fd_, data.data(), data.size());
  Log_Assert(result == (int)data.size(), "Failed to write to file.");
}
