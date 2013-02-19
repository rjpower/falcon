#ifndef TEMPEST_UTIL_H
#define TEMPEST_UTIL_H

#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>

#include <boost/function.hpp>

#include <map>
#include <string>
#include <vector>

static __attribute__((always_inline)) inline void breakpoint() {
  struct sigaction oldAct;
  struct sigaction newAct;
  newAct.sa_handler = SIG_IGN;
  sigaction(SIGTRAP, &newAct, &oldAct);
  raise(SIGTRAP);
  sigaction(SIGTRAP, &oldAct, NULL);
}

static inline uint64_t rdtsc() {
  uint32_t hi, lo;
  __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
  return (((uint64_t) hi) << 32) | ((uint64_t) lo);
}

double Now();
std::string Hostname();
timeval timevalFromDouble(double t);
timespec timespecFromDouble(double t);

void Sleep(double sleepTime);

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
private:
  const char* data_;
  int len_;
};

bool operator==(const StringPiece& a, const char* b);
bool operator==(const StringPiece& a, const StringPiece& b);

struct StrUtil {
  static std::vector<StringPiece> split(StringPiece sp, StringPiece delim);

  template<class Iterator>
  static std::string join(Iterator start, Iterator end, std::string delim = " ");

  template<class Iterator, class Converter>
  static std::string join(Iterator start, Iterator end, std::string delim, Converter to_str);

  template<class ValueType>
  static std::string join(const std::vector<ValueType>& v, std::string delim);
};

const char* strnstr(const char* haystack, const char* needle, int len);

std::string StringPrintf(StringPiece fmt, ...);
std::string VStringPrintf(StringPiece fmt, va_list args);

struct Counters {
  typedef std::map<std::string, double*> CounterMap;
  static CounterMap counters;
  static double& get(const std::string& name) {
    if (counters.find(name) == counters.end()) {
      counters[name] = new double(0);
    }
    return *counters[name];
  }

  static std::string str() {
    std::string out;
    for (CounterMap::iterator i = counters.begin(); i != counters.end(); ++i) {
      out += StringPrintf("%s : %.2f, ", i->first.c_str(), *i->second);
    }
    return out;
  }
};

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

struct Coerce {
  static std::string str(const size_t& v) {
    return str((int)v);
  }

  static std::string str(const long& v) {
    return StringPrintf("%ld", v);
  }

  static std::string str(const short & v);
  static std::string str(const int& v);
  static std::string str(const double& v);
  static std::string str(const std::string& v);

  template<class T>
  static std::string str(const T* t);

  template<class T>
  static std::string t_str(const T& v);

  template<class T>
  static std::string str(const std::vector<T>& v);

  template<class K, class V>
  static std::string str(const std::map<K, V>& m);
};

class Writer {
public:
  virtual void printf(const char* fmt, ...);
  virtual void write(const char*);

  virtual void write(const std::string&) = 0;

  template<class T>
  void write(const T& t) {
    write(Coerce::t_str(t));
  }
};

class StringWriter: public Writer {
  std::string buffer_;
public:
  virtual void write(const std::string& data);
  const std::string& str();
};

class FileWriter: public Writer {
private:
  int fd_;
public:
  FileWriter(int fd) :
      fd_(fd) {
  }
  virtual void write(const std::string& data);
};

enum LogLevel {
  kLogDebug = 0,
  kLogInfo = 1,
  kLogWarn = 2,
  kLogError = 3,
  kLogFatal = 4,
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

#define LOG_IF(level, fmt, ...)\
  do {\
    if (level >= currentLogLevel) {\
      logAtLevel(level, __FILE__, __LINE__, fmt, ##__VA_ARGS__);\
    }\
  } while (0)

#define Log_Debug(fmt, ...) LOG_IF(kLogDebug, fmt, ##__VA_ARGS__)
#define Log_Info(fmt, ...) LOG_IF(kLogInfo, fmt, ##__VA_ARGS__)
#define Log_Warn(fmt, ...) LOG_IF(kLogWarn, fmt, ##__VA_ARGS__)
#define Log_Error(fmt, ...) LOG_IF(kLogError, fmt, ##__VA_ARGS__)
#define Log_Fatal(fmt, ...) LOG_IF(kLogFatal, fmt, ##__VA_ARGS__)

#define Log_Perror(fmt, ...)\
  Log_Warn("%s :: (System error: %s)", StringPrintf(fmt, ##__VA_ARGS__).c_str(), strerror(errno));

#define Log_PAssert(expr, fmt, ...)\
  if (!(expr)) { Log_Fatal("%s :: (System error: %s)", StringPrintf(fmt, ##__VA_ARGS__).c_str(), strerror(errno)); }

#define Log_Assert(expr, fmt, ...)\
  if(!(expr)) { Log_Fatal(fmt, ##__VA_ARGS__); }

template<class K, class V>
inline std::string Coerce::str(const std::map<K, V>& m) {
  StringWriter w;
  w.write("{\n");
  for (auto i = m.begin(); i != m.end(); ++i) {
    w.printf("%s : %s,\n", Coerce::str(i->first).c_str(), Coerce::str(i->second).c_str());
  }
  w.write("}\n");
  return w.str();
}

template<class T>
inline std::string Coerce::str(const std::vector<T>& v) {
  return StrUtil::join(v.begin(), v.end(), ",");
}

template<class T>
inline std::string Coerce::t_str(const T& v) {
  return str(v);
}

template<class T>
inline std::string Coerce::str(const T* t) {
  return t->str();
}

template<class Iterator>
inline std::string StrUtil::join(Iterator start, Iterator end, std::string delim) {
  std::string out;
  while (start != end) {
    out += Coerce::str(*start);
    ++start;
    if (start != end) {
      out += delim;
    }
  }
  return out;
}

template<class Iterator, class Converter>
inline std::string StrUtil::join(Iterator start, Iterator end, std::string delim, Converter to_str) {
  std::string out;
  while (start != end) {
    std::string part = to_str(*start);
    out += part;
    ++start;
    if (start != end) {
      out += delim;
    }
  }
  return out;
}

template<class ValueType>
inline std::string StrUtil::join(const std::vector<ValueType>& v, std::string delim) {
  return join(v.begin(), v.end(), delim);
}

#endif
