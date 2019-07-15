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

namespace vst {

#ifdef _WIN32
std::wstring widen(const std::string& s);
std::string shorten(const std::wstring& s);
#endif

std::string expandPath(const char *path);

// cross platform fstream
class File : public std::fstream {
public:
#ifdef _WIN32
    File(const std::wstring& path)
    // supported by MSVC and newer GCC versions
#if defined(_MSC_VER) || __GNUC__ >= 9
        : std::fstream(path.c_str(), std::ios::binary | std::ios::in | std::ios::out),
          path_(path){}
#else
        : std::fstream(shorten(path), std::ios::binary | std::ios::in | std::ios::out),
          path_(path){} // might create problems...
#endif // MSVC/GCC9
#else
    File(const std::string& path)
        : std::fstream(path, std::ios::binary | std::ios::in | std::ios::out),
          path_(path){}
#endif
protected:
#ifdef _WIN32
    std::wstring path_;
#else
    std::string path_;
#endif
};

} // vst
