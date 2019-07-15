#pragma once

#include <iostream>
#include <fstream>

	// log level: 0 (error), 1 (warning), 2 (verbose), 3 (debug)
#ifndef LOGLEVEL
#define LOGLEVEL 0
#endif

#ifdef LOGFUNCTION
#include <sstream>
	void LOGFUNCTION(const std::string& msg);
	class Log {
	public:
		~Log() {
			stream_ << "\n";
			std::string msg = stream_.str();
			LOGFUNCTION(msg.c_str());
		}
		template<typename T>
		Log& operator<<(T&& t) {
            stream_ << std::forward<T>(t);
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

namespace vst {

#ifdef _WIN32
std::wstring widen(const std::string& s);
std::string shorten(const std::wstring& s);
#endif

std::string expandPath(const char *path);

// cross platform fstream, taking UTF-8 file paths.
// will become obsolete when we can switch the whole project to C++17
class File : public std::fstream {
public:
    File(const std::string& path)
#if defined(_WIN32) && (defined(_MSC_VER) || __GNUC__ >= 9)
    // UTF-16 file names supported by MSVC and newer GCC versions
        : std::fstream(vst::widen(path).c_str(),
#else
    // might create problems on Windows... LATER fix this
        : std::fstream(path,
#endif
                       ios_base::binary | ios_base::in | ios_base::out | ios_base::app),
          path_(path){}
protected:
    std::string path_;
};

} // vst
