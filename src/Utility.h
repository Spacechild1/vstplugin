#pragma once

#include <iostream>

// log level: 0 (error), 1 (warning), 2 (verbose), 3 (debug)
#ifndef LOGLEVEL
#define LOGLEVEL 0
#endif

#ifdef LOGFUNCTION
#include <sstream>
void LOGFUNCTION(const std::string& msg);

class Log {
public:
	~Log(){
		stream_ << "\n";
		std::string msg = stream_.str();
		LOGFUNCTION(msg.c_str());
	}
	template<typename T>
	Log& operator<<(T&& t){
		stream_ << std::move(t);
        return *this;
	}
private:
	std::ostringstream stream_;
};
#define DO_LOG(x) (Log() << x)
#else // default log function
#define DO_LOG(x) std::cerr << x << std::endl
#endif

#if LOGLEVEL >= 0
#define LOG_ERROR(x) DO_LOG(x)
#else
#define LOG_ERROR(x)
#endif

#if LOGLEVEL >= 1
#define LOG_WARNING(x) DO_LOG(x)
#else
#define LOG_WARNING(x)
#endif

#if LOGLEVEL >= 2
#define LOG_VERBOSE(x) DO_LOG(x)
#else
#define LOG_VERBOSE(x)
#endif

#if LOGLEVEL >= 3
#define LOG_DEBUG(x) DO_LOG(x)
#else
#define LOG_DEBUG(x)
#endif
