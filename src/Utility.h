#pragma once

#include <iostream>

// log level: 0 (error), 1 (warning), 2 (verbose), 3 (debug)
#ifndef LOGLEVEL
#define LOGLEVEL 0
#endif

#if LOGLEVEL >= 0
#define LOG_ERROR(x) (std::cerr << x << std::endl)
#else
#define LOG_ERROR(x)
#endif

#if LOGLEVEL >= 1
#define LOG_WARNING(x) (std::cerr << x << std::endl)
#else
#define LOG_WARNING(x)
#endif

#if LOGLEVEL >= 2
#define LOG_VERBOSE(x) (std::cerr << x << std::endl)
#else
#define LOG_VERBOSE(x)
#endif

#if LOGLEVEL >= 3
#define LOG_DEBUG(x) (std::cerr << x << std::endl)
#else
#define LOG_DEBUG(x)
#endif
