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
#include "Log.h"

#include <string>
#include <cstring>
#include <stdlib.h>

namespace  vst {

#ifdef __APPLE__
// apparently, aligned default delete is only available on macOS 10.14+,
// so we need to manually override operator new and operator delete!
template<typename T>
class AlignedClass {
public:
    void* operator new(size_t size) {
        void *ptr = nullptr;
        if (posix_memalign(&ptr, alignof(T), size) != 0) {
            LOG_WARNING("posix_memalign() failed");
            ptr = std::malloc(size);
            if (!ptr) {
                throw std::bad_alloc();
            }
        }
        return ptr;
    }

    void operator delete(void* ptr) {
        std::free(ptr);
    }

    void* operator new[](size_t size) {
        return operator new(size);
    }

    void operator delete[](void *ptr) {
        return operator delete(ptr);
    }
};
#else
template<typename T>
class AlignedClass {};
#endif

//---------------------------------------------------------------//

// NB: requires CTAD (C++17 and above)
template<typename T>
class ScopeGuard {
public:
    ScopeGuard(const T& fn)
        : fn_(fn) {}

    ~ScopeGuard() { fn_(); }
private:
    T fn_;
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

//------------------- string utilities --------------------------//

#ifdef _WIN32
std::wstring widen(std::string_view s);
std::string shorten(std::wstring_view s);
#else
// No-op versions. Yes, this creates unnecessary copies, but this does not really
// matter in the context where these functions are typically used (file I/O).
// NB: we cannot use a macro because it conflicts with the widen() function in the stdlib.
inline std::string widen(std::string_view s) { return std::string{s}; }
inline std::string shorten(std::string_view s) { return std::string{s}; }
#endif

inline bool startsWith(std::string_view s, std::string_view s2) {
    if (s.size() >= s2.size()) {
        return memcmp(s.data(), s2.data(), s2.size()) == 0;
    } else {
        return false;
    }
}

// lexicographical case-insensitive string comparison function
inline bool stringCompare(std::string_view lhs, std::string_view rhs) {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](const auto& c1, const auto& c2){ return std::tolower(c1) < std::tolower(c2); });
}

//------------------- system utilities --------------------------//

std::string errorMessage(int err);

std::string getTmpDirectory();

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

int runCommand(std::string_view cmd, std::string_view args);

enum class Priority {
    Low,
    Normal,
    High
};

void setThreadPriority(Priority p);

} // vst
