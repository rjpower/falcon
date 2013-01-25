#ifndef TEMPEST_UTIL_H
#define TEMPEST_UTIL_H

#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>

#include <map>
#include <string>
#include <vector>

extern "C" {
void breakpoint();
const char* opcode_to_name(int opcode);
}

static inline uint64_t rdtsc() {
  uint32_t hi, lo;
  __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
  return (((uint64_t) hi) << 32) | ((uint64_t) lo);
}

class StringPiece {
public:
  StringPiece();
  StringPiece(const StringPiece& s);
  StringPiece(const std::string& s);
  StringPiece(const std::string& s, int len);
  StringPiece(const char* c);
  StringPiece(const char* c, int len);

  // Remove whitespace from either side
  void strip();

  std::string AsString() const;
  std::string str() const {
    return AsString();
  }

  int size() const {
    return len_;
  }

  const char* data() const {
    return data_;
  }

  static std::vector<StringPiece> split(StringPiece sp, StringPiece delim);
private:
  const char* data_;
  int len_;
};

bool operator==(const StringPiece& a, const char* b);
bool operator==(const StringPiece& a, const StringPiece& b);

const char* strnstr(const char* haystack, const char* needle, int len);

std::string StringPrintf(StringPiece fmt, ...);
std::string VStringPrintf(StringPiece fmt, va_list args);

std::string ToString(int32_t);
std::string ToString(int64_t);
std::string ToString(double);
std::string ToString(std::string);
std::string ToString(StringPiece);

template<class T>
std::string ToString(const std::vector<T>& v) {
  return JoinString(v.begin(), v.end(), ",");
}

template<class A, class B>
std::string ToString(const std::pair<A, B>& v) {
  return ToString(v.first) + " : " + ToString(v.second);
}

template<class Iterator>
std::string JoinString(Iterator start, Iterator end, std::string delim = " ") {
  std::string out;
  while (start != end) {
    out += ToString(*start);
    ++start;
    if (start != end) {
      out += delim;
    }
  }
  return out;
}

struct TimerRegistry {
  typedef std::map<std::string, double*> TimerMap;
  static TimerMap timers;
  static double& get(const std::string& name) {
    if (timers.find(name) == timers.end()) {
      timers[name] = new double(0);
    }
    return *timers[name];
  }

  static std::string str() {
    std::string out;
    for (TimerMap::iterator i = timers.begin(); i != timers.end(); ++i) {
      out += StringPrintf("%s : %.2f, ", i->first.c_str(), *i->second);
    }
    return out;
  }
};

double Now();
std::string Hostname();
timeval timevalFromDouble(double t);
timespec timespecFromDouble(double t);

struct TimerBlock {
  double& t_;
  double start_;
  TimerBlock(double& t) :
      t_(t), start_(Now()) {
  }
  ~TimerBlock() {
    t_ += Now() - start_;
  }
};

void Sleep(double sleepTime);

enum LogLevel {
  kLogDebug = 0, kLogInfo = 1, kLogWarn = 2, kLogError = 3, kLogFatal = 4,
};

extern LogLevel currentLogLevel;
double get_processor_frequency();

#define EVERY_N(interval, operation)\
{ static int COUNT = 0;\
  if (COUNT++ % interval == 0) {\
    operation;\
  }\
}

#define START_PERIODIC(interval)\
{ static int64_t last = 0;\
  static int64_t cycles = (int64_t)(interval * get_processor_frequency());\
  static int COUNT = 0; \
  ++COUNT; \
  int64_t now = rdtsc(); \
  if (now - last > cycles) {\
    last = now;\
    COUNT = 0;

#define END_PERIODIC() } }

#define PERIODIC(interval, op)\
    START_PERIODIC(interval)\
    op;\
    END_PERIODIC()

void logAtLevel(LogLevel level, const char* file, int line, const char* fmt, ...);

#define Log_Debug(fmt, ...) logAtLevel(kLogDebug, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define Log_Info(fmt, ...) logAtLevel(kLogInfo, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define Log_Warn(fmt, ...) logAtLevel(kLogWarn, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define Log_Error(fmt, ...) logAtLevel(kLogError, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define Log_Fatal(fmt, ...) logAtLevel(kLogFatal, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define Log_Perror(fmt, ...)\
  Log_Warn("%s :: (System error: %s)", StringPrintf(fmt, ##__VA_ARGS__).c_str(), strerror(errno));

#define Log_PAssert(expr, fmt, ...)\
  if (!(expr)) { Log_Fatal("%s :: (System error: %s)", StringPrintf(fmt, ##__VA_ARGS__).c_str(), strerror(errno)); }

#define Log_Assert(expr, fmt, ...)\
  if(!(expr)) { Log_Fatal(fmt, ##__VA_ARGS__); }

#endif
