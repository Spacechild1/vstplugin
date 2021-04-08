cmake_minimum_required (VERSION 2.8)

set(PROJECT "vstplugin")
message(STATUS "Project: ${PROJECT}")
project(${PROJECT})

include (CheckCCompilerFlag)
include (CheckCXXCompilerFlag)

if(UNIX AND NOT APPLE AND NOT MINGW)
	set(LINUX TRUE)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	set(CMAKE_COMPILER_IS_CLANG 1)
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

if(LINUX AND CMAKE_COMPILER_IS_GNUCXX)
	option(STATIC_LIBS "link with static libraries (libstdc++ and libgcc)" ON)
endif()
if(MINGW)
	option(STATIC_LIBS "link with static libraries (libstdc++, libgcc and phread)" ON)
	set(CMAKE_EXECUTABLE_SUFFIX ".exe")
endif()

set(CMAKE_INSTALL_PREFIX "" CACHE INTERNAL "Prefix prepended to install directories")

# use std::filesystem?
if(WIN32 OR MINGW OR BUILD_WINEHOST)
    set(STDFS ON CACHE BOOL "Use std::filesystem")
else()
    set(STDFS OFF CACHE BOOL "Use std::filesystem")
endif()
add_definitions(-DUSE_STDFS=$<BOOL:${STDFS}>)

# Windows paths
if (WIN32 OR MINGW)
    # check if "Program Files (x86)" exists (64-bit Windows) and if we compile for 32-bit
    set(_pf_x86 "ProgramFiles(x86)")
    if (DEFINED ENV{${_pf_x86}} AND (CMAKE_SIZEOF_VOID_P EQUAL 4))
        set(PROGRAMFILES $ENV{${_pf_x86}})
    else()
        set(PROGRAMFILES $ENV{PROGRAMFILES})
    endif()
    set(APPDATA $ENV{APPDATA})
    set(LOCALAPPDATA $ENV{LOCALAPPDATA})
endif()

# logging
set(LOGLEVEL 2 CACHE STRING "LOGLEVEL")
message(STATUS "LOGLEVEL: ${LOGLEVEL}")
add_definitions(-DLOGLEVEL=${LOGLEVEL})

# compiler flags
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_COMPILER_IS_CLANG)
    add_definitions(-fvisibility=hidden)
    # disable some warnings
    add_definitions(-Wno-unknown-pragmas -Wno-format-security)

    CHECK_CXX_COMPILER_FLAG(-msse HAS_CXX_SSE)
    if (HAS_CXX_SSE)
        add_definitions("-msse")
    endif()

    CHECK_CXX_COMPILER_FLAG(-msse2 HAS_CXX_SSE2)
    if (HAS_CXX_SSE2)
        add_definitions("-msse2")
    endif()

    CHECK_CXX_COMPILER_FLAG(-msse3 HAS_CXX_SSE3)
    if (HAS_CXX_SSE3)
        add_definitions("-msse3")
    endif()

    # some people might still use old AMD CPUs...
    if (OFF)
        CHECK_CXX_COMPILER_FLAG(-msse4 HAS_CXX_SSE4)
        if (HAS_CXX_SSE4)
            add_definitions("-msse4")
        endif()
    endif()

    CHECK_CXX_COMPILER_FLAG(-mfpmath=sse HAS_CXX_FPMATH_SSE)
    if (HAS_CXX_FPMATH_SSE)
        add_definitions("-mfpmath=sse")
    endif()

    if(NATIVE)
        add_definitions(-march=native)
    endif()

    add_definitions("-std=c++14 -ffast-math -funroll-loops -fomit-frame-pointer")

    if(CMAKE_COMPILER_IS_CLANG)
        add_definitions("-stdlib=libc++")
    endif()
endif()
if(MINGW)
    set(CMAKE_CXX_COMPILER g++)
    add_definitions("-mstackrealign")
endif()

# include dependencies
include_directories(deps)

# VST2 SDK
option(VST2 "Enable VST2.x plug-ins" ON)
add_definitions(-DUSE_VST2=$<BOOL:${VST2}>)
if (VST2)
    set(VST2DIR "${CMAKE_SOURCE_DIR}/vst/VST_SDK/VST2_SDK/" CACHE PATH "path to VST2_SDK")
    message(STATUS "VST2DIR: ${VST2DIR}")
endif()
option(FST "Use FST instead of the official VST2 SDK")
if (FST)
    add_definitions(-DUSE_FST=1)
    message(STATUS "Using FST")
endif()

# VST3 SDK
option(VST3 "Enable VST3 plug-ins" ON)
add_definitions(-DUSE_VST3=$<BOOL:${VST3}>)
if (VST3)
    set(VST3DIR "${CMAKE_SOURCE_DIR}/vst/VST_SDK/VST3_SDK/" CACHE PATH "path to VST3_SDK")
    message(STATUS "VST3DIR: ${VST3DIR}")
endif()

# bit bridging
option(BRIDGE "Enable plugin bridge" ON)
add_definitions(-DUSE_BRIDGE=$<BOOL:${BRIDGE}>)
if (BRIDGE)
    message(STATUS "Enable plugin bridge")
    if(APPLE OR LINUX)
        # Try to directly build the 32-bit (Intel) version of 'HOST' with -m32.
        # This works on Linux and older macOS versions.
        # We don't enable this by default because on Linux we would have to install
        # lots of extra libs and on macOS it won't work on newer OS versions (macOs 10.15+) anyway.
        # (Maybe we can somehow detect the macOs version)?
        #
        # NOTE: I've tried to use CHECK_CXX_COMPILER_FLAG(-m32 HAS_CXX_M32) but it always fails...
        option(BUILD_HOST32 "Build the 32-bit host app" OFF)
    endif()
endif()

# Wine support on Linux
if(NOT (WIN32 OR MINGW))
    option(WINE "Enable support for running Windows VSTs with Wine" ON)
    if (WINE)
        if (BRIDGE)
            message(STATUS "Enable Wine support")
        else()
            message(WARNING "Wine support doesn't work without plugin bridge (BRIDGE)")
        endif()
    endif()
    add_definitions(-DUSE_WINE=$<BOOL:${WINE}>)
endif()

if (STDFS AND (LINUX OR MINGW OR BUILD_WINEHOST))
    list(APPEND VST_LIBS "-lstdc++fs")
endif()

# platform specific linker flags
if (LINUX)
    list(APPEND VST_LIBS "-ldl" "-L/usr/X11R6/lib" "-lX11" "-pthread")
    if (BRIDGE)
        # for shm_open/shm_unlink
        list(APPEND VST_LIBS "-lrt")
    endif()
    if(STATIC_LIBS)
        list(APPEND VST_LIBS "-static-libstdc++" "-static-libgcc")
    endif()
    add_definitions("-fPIC")
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
endif()
if (MINGW)
    if (STATIC_LIBS)
        list(APPEND VST_LIBS "-static-libstdc++" "-static-libgcc" "-static -lpthread")
    else()
        list(APPEND VST_LIBS "-lpthread")
    endif()
endif()
if (APPLE)
    list(APPEND VST_LIBS "-framework Cocoa" "-lpthread")
endif()

# vst library
include_directories(vst)

set(VST "vst")
set(LIBS ${VST} ${VST_LIBS})
if (BUILD_HOST32)
    set(VST32 "vst32")
    set(LIBS32 ${VST32} ${VST_LIBS})
endif()

add_subdirectory(vst)

# host exe
set(HOST "host")
add_executable(${HOST} "vst/${HOST}.cpp") # HOST exe
target_link_libraries(${HOST} ${LIBS})
set_target_properties(${HOST} PROPERTIES LINK_FLAGS_RELEASE -s)
if (MINGW)
    # -municode for wmain and -mwindows to hide console window
    target_link_libraries(${HOST} -municode)
endif()
# up to CMake 3.12 we can't install targets from another directory,
# so we export the output file path and install it as a file.
set(HOST_PATH $<TARGET_FILE:${HOST}>)

if (BUILD_HOST32)
    message(STATUS "Build 32-bit host program")
    # is -m32 always Intel?
    set(HOST32 "host_i386")
    add_executable(${HOST32} "vst/${HOST}.cpp")
    target_compile_options(${HOST32} PUBLIC "-m32")
    target_link_options(${HOST32} PUBLIC "-m32")
    target_link_libraries(${HOST32} ${LIBS32})
    set_target_properties(${HOST32} PROPERTIES LINK_FLAGS_RELEASE -s)
    # see above
    set(HOST32_PATH $<TARGET_FILE:${HOST32}>)
endif()

# vstplugin~
option(PD "build Pd external" ON)
if (PD)
    add_subdirectory(pd)
endif()

# VSTPlugin
option(SC "build SC plugin" ON)
if (SC)
    add_subdirectory(sc)
endif()

# testsuite
option(TESTSUITE "build test suite" ON)
if (TESTSUITE)
    add_subdirectory(test)
endif()
