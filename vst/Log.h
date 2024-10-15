#pragma once

#include "Interface.h"

#include <sstream>

// log levels:
// NB: log levels must be preprocessor defines!
#define LOG_LEVEL_SILENT 0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARNING 2
#define LOG_LEVEL_INFO 3
#define LOG_LEVEL_DEBUG 4
#define LOG_LEVEL_VERBOSE 5

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_SILENT
#endif

namespace vst {

void logMessage(int level, const char* msg);

void logMessage(int level, std::string_view msg);

class Log {
public:
    Log(int level = LOG_LEVEL_INFO) : level_(level) {}
    ~Log() {
        stream_ << "\n";
        logMessage(level_, stream_.str());
    }
    template<typename T>
    Log& operator<<(T&& t) {
        stream_ << std::forward<T>(t);
        return *this;
    }
private:
    std::ostringstream stream_;
    int level_;
};

// for interprocess logging
struct LogMessage {
    struct Header {
        int32_t level;
        int32_t size;
    };

    Header header;
    char data[1];
};

} // vst

#define DO_LOG(level, x) do { vst::Log(level) << x; } while(false)

#if LOG_LEVEL >= LOG_LEVEL_ERROR
#define LOG_ERROR(x) DO_LOG(LOG_LEVEL_ERROR, x)
#else
#define LOG_ERROR(x)
#endif

#if LOG_LEVEL >= LOG_LEVEL_WARNING
#define LOG_WARNING(x) DO_LOG(LOG_LEVEL_WARNING, x)
#else
#define LOG_WARNING(x)
#endif

#if LOG_LEVEL >= LOG_LEVEL_INFO
#define LOG_INFO(x) DO_LOG(LOG_LEVEL_INFO, x)
#else
#define LOG_INFO(x)
#endif

#if LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_DEBUG(x) DO_LOG(LOG_LEVEL_DEBUG, x)
#else
#define LOG_DEBUG(x)
#endif

#if LOG_LEVEL >= LOG_LEVEL_VERBOSE
#define LEVEL_VERBOSE(x) DO_LOG(LOG_LEVEL_VERBOSE, x)
#else
#define LEVEL_VERBOSE(x)
#endif
