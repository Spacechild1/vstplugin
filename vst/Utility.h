#pragma once

#ifdef _WIN32
# include <malloc.h> // MSVC or mingw on windows
# ifdef _MSC_VER
#  define alloca _alloca
# endif
#elif defined(__linux__) || defined(__APPLE__)
# include <alloca.h> // linux, mac, mingw, cygwin
#else
# include <stdlib.h> // BSDs for example
#endif

/*------------------ endianess -------------------*/
    // endianess check taken from Pure Data (d_osc.c)
#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__FreeBSD_kernel__) \
    || defined(__OpenBSD__)
#include <machine/endian.h>
#endif

#if defined(__linux__) || defined(__CYGWIN__) || defined(__GNU__) || \
    defined(ANDROID)
#include <endian.h>
#endif

#ifdef __MINGW32__
#include <sys/param.h>
#endif

#ifdef _MSC_VER
/* _MSVC lacks BYTE_ORDER and LITTLE_ENDIAN */
#define LITTLE_ENDIAN 0x0001
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#if !defined(BYTE_ORDER) || !defined(LITTLE_ENDIAN)
#error No byte order defined
#endif

#include <iostream>
#include <fstream>
#include <atomic>
#include <array>

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

// lexicographical case-insensitive string comparison function
bool stringCompare(const std::string& lhs, const std::string& rhs);

std::string expandPath(const char *path);

bool pathExists(const std::string& path);

bool isDirectory(const std::string& path);

bool isFile(const std::string& path);

bool removeFile(const std::string& path);

bool renameFile(const std::string& from, const std::string& to);

bool createDirectory(const std::string& dir);

std::string fileName(const std::string& path);

std::string fileExtension(const std::string& path);

std::string fileBaseName(const std::string& path);

std::string errorMessage(int err);

//--------------------------------------------------------------------------------------------------------

// cross platform fstream, taking UTF-8 file paths.
// will become obsolete when we can switch the whole project to C++17
class File : public std::fstream {
public:
    enum Mode {
        READ,
        WRITE
    };
    File(const std::string& path, Mode mode = READ)
#if defined(_WIN32) && (defined(_MSC_VER) || __GNUC__ >= 9)
    // UTF-16 file names supported by MSVC and newer GCC versions
        : std::fstream(vst::widen(path).c_str(),
#else
    // might create problems on Windows... LATER fix this
        : std::fstream(path,
#endif
                       ios_base::binary |
                       (mode == READ ? ios_base::in : (ios_base::out | ios_base::trunc))),
          path_(path){}
protected:
    std::string path_;
};

//--------------------------------------------------------------------------------------------------------

enum class ThreadPriority {
    Low,
    High
};

void setThreadPriority(ThreadPriority p);

//--------------------------------------------------------------------------------------------------------

template<typename T, size_t N>
class LockfreeFifo {
 public:
    template<typename... TArgs>
    bool emplace(TArgs&&... args){
        return push(T{std::forward<TArgs>(args)...});
    }
    bool push(const T& data){
        int next = (writeHead_.load(std::memory_order_relaxed) + 1) % N;
        if (next == readHead_.load(std::memory_order_relaxed)){
            return false; // FIFO is full
        }
        data_[next] = data;
        writeHead_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& data){
        int pos = readHead_.load(std::memory_order_relaxed);
        if (pos == writeHead_.load()){
            return false; // FIFO is empty
        }
        int next = (pos + 1) % N;
        data = data_[next];
        readHead_.store(next, std::memory_order_release);
        return true;
    }
    void clear() {
        readHead_.store(writeHead_.load());
    }
    bool empty() const {
        return readHead_.load(std::memory_order_relaxed) == writeHead_.load(std::memory_order_relaxed);
    }
    size_t capacity() const { return N; }
    // raw data
    int readPos() const { return readHead_.load(std::memory_order_relaxed); }
    int writePos() const { return writeHead_.load(std::memory_order_relaxed); }
    T * data() { return data_.data(); }
    const T* data() const { return data_.data(); }
 private:
    std::atomic<int> readHead_{0};
    std::atomic<int> writeHead_{0};
    std::array<T, N> data_;
};

} // vst
