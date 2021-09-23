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

namespace  vst {

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

std::string getHostApp(CpuArch arch);

#ifdef _WIN32
const std::wstring& getModuleDirectory();
#else
const std::string& getModuleDirectory();
#endif

#if USE_WINE
const char * getWineCommand();

const char * getWineFolder();

bool haveWine();
#endif

#if USE_BRIDGE
bool canBridgeCpuArch(CpuArch arch);
#endif

int runCommand(const char *cmd, const char *args);

//---------------------------------------------------------------//

enum class Priority {
    Low,
    Normal,
    High
};

void setThreadPriority(Priority p);

} // vst
