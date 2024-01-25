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

#include "Interface.h"

#include <string>
#include <cstring>

namespace  vst {

// TODO: with C++17 we cold directly store a lambda because
// of class template argument deduction in constructors.
class ScopeGuard {
public:
    ~ScopeGuard() { fn_(); }

    template<typename Func>
    ScopeGuard(const Func& fn)
        : fn_(fn) {}
private:
    std::function<void()> fn_;
};

//--------------------------------------------------------------//

template<typename T>
constexpr bool isPowerOfTwo(T v) {
    return (v & (v - 1)) == 0;
}

template<typename T>
constexpr T nextPowerOfTwo(T v) {
    T result = 1;
    while (result < v) {
        result *= 2;
    }
    return result;
}

template<typename T>
constexpr T prevPowerOfTwo(T v) {
    T result = 1;
    while (result <= v) {
        result *= 2;
    }
    return result >> 1;
}

template<typename T>
T alignTo(T v, size_t alignment) {
    auto mask = alignment - 1;
    return (v + mask) & ~mask;
}

inline bool startsWith(const std::string& s, const char *c, size_t n) {
    if (s.size() >= n) {
        return memcmp(s.data(), c, n) == 0;
    } else {
        return false;
    }
}

inline bool startsWith(const std::string& s, const char *s2) {
    return startsWith(s, s2, strlen(s2));
}

inline bool startsWith(const std::string& s, const std::string& s2) {
    return startsWith(s, s2.data(), s2.size());
}

//--------------------------------------------------------------//

#ifdef _WIN32
std::wstring widen(const std::string& s);

std::string shorten(const std::wstring& s);
#endif

std::string getTmpDirectory();

// lexicographical case-insensitive string comparison function
bool stringCompare(const std::string& lhs, const std::string& rhs);

std::string errorMessage(int err);

//---------------------------------------------------------------//

const std::string& getModuleDirectory();

void * getModuleHandle();

#if USE_WINE
const char * getWineCommand();

const char * getWine64Command();

const char * getWineFolder();

bool haveWine();

bool haveWine64();
#endif

int getCurrentProcessId();

int runCommand(const std::string& cmd, const std::string& args);

enum class Priority {
    Low,
    Normal,
    High
};

void setThreadPriority(Priority p);

} // vst
