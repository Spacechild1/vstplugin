#pragma once

#include <sstream>

// log level: 0 (error), 1 (warning), 2 (verbose), 3 (debug)
#ifndef LOGLEVEL
#define LOGLEVEL 0
#endif

namespace vst {

class Log {
public:
    Log(int level = LOGLEVEL) : level_(level) {}
    ~Log();
    template<typename T>
    Log& operator<<(T&& t) {
        stream_ << std::forward<T>(t);
        return *this;
    }
private:
    std::ostringstream stream_;
    int level_;
};

} // vst

#define DO_LOG(level, x) do { vst::Log(level) << x; } while(false)

#if LOGLEVEL >= 0
#define LOG_ERROR(x) DO_LOG(0, x)
#else
#define LOG_ERROR(x)
#endif

#if LOGLEVEL >= 1
#define LOG_WARNING(x) DO_LOG(1, x)
#else
#define LOG_WARNING(x)
#endif

#if LOGLEVEL >= 2
#define LOG_VERBOSE(x) DO_LOG(2, x)
#else
#define LOG_VERBOSE(x)
#endif

#if LOGLEVEL >= 3
#define LOG_DEBUG(x) DO_LOG(3, x)
#else
#define LOG_DEBUG(x)
#endif
