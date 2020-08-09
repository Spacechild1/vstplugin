cmake_minimum_required (VERSION 2.8)

set(PROJECT "vstplugin")
message(STATUS "Project: ${PROJECT}")
project(${PROJECT})

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(MARCH64 TRUE)
endif()

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

# Windows paths
if (WIN32 OR MINGW)
    # check if "Program Files (x86)" exists (64-bit Windows) and if we compile for 32-bit
    set(_pf_x86 "ProgramFiles(x86)")
    if (DEFINED ENV{${_pf_x86}} AND NOT MARCH64)
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

    include (CheckCCompilerFlag)
    include (CheckCXXCompilerFlag)

    CHECK_CXX_COMPILER_FLAG(-msse HAS_CXX_SSE)
    if (HAS_CXX_SSE)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse")
    endif()

    CHECK_CXX_COMPILER_FLAG(-msse2 HAS_CXX_SSE2)
    if (HAS_CXX_SSE2)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse2")
    endif()

    CHECK_CXX_COMPILER_FLAG(-msse3 HAS_CXX_SSE3)
    if (HAS_CXX_SSE3)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse3")
    endif()

    CHECK_CXX_COMPILER_FLAG(-msse4 HAS_CXX_SSE4)
    if (HAS_CXX_SSE4)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse4")
    endif()

    CHECK_CXX_COMPILER_FLAG(-mfpmath=sse HAS_CXX_FPMATH_SSE)
    if (HAS_CXX_FPMATH_SSE)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mfpmath=sse")
    endif()

    if(NATIVE)
        add_definitions(-march=native)
    endif()

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -ffast-math -funroll-loops -fomit-frame-pointer")

    if(CMAKE_COMPILER_IS_CLANG)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
    endif()
endif()
if(MINGW)
    set(CMAKE_CXX_COMPILER g++)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mstackrealign")
endif()

# include dependencies
include_directories(deps)

# VST2 SDK
option(VST2 "Enable VST2.x plug-ins" ON)
if (VST2)
    set(VST2DIR "${CMAKE_SOURCE_DIR}/vst/VST_SDK/VST2_SDK/" CACHE PATH "path to VST2_SDK")
    add_definitions(-DUSE_VST2=1)
    message(STATUS "VST2DIR: ${VST2DIR}")
endif()
option(FST "Use FST instead of the official VST2 SDK")
if (FST)
    add_definitions(-DUSE_FST=1)
    message(STATUS "Using FST")
endif()

# VST3 SDK
option(VST3 "Enable VST3 plug-ins" ON)
if (VST3)
    set(VST3DIR "${CMAKE_SOURCE_DIR}/vst/VST_SDK/VST3_SDK/" CACHE PATH "path to VST3_SDK")
    add_definitions(-DUSE_VST3=1)
    message(STATUS "VST3DIR: ${VST3DIR}")
endif()

# bit bridging
option(BRIDGE "Enable bit-bridging" ON)
if (BRIDGE)
    add_definitions(-DUSE_BRIDGE=1)
    if (MARCH64 AND APPLE)
        # on 64-bit macOS we can directly build the 32-bit version of 'HOST'.
        # we assume that the architecture is Intel. LATER add a check for ARM!
        set(HOST32 "host_i386")
    endif()
    if (LINUX)
        # for shm_open/shm_unlink
        list(APPEND VST_LIBS "-lrt")
    endif()
endif()

# vst library
include_directories(vst)

# platform specific linker flags
if (LINUX)
    list(APPEND VST_LIBS "-ldl" "-L/usr/X11R6/lib" "-lX11" "-pthread")
    if(STATIC_LIBS)
        list(APPEND VST_LIBS "-static-libstdc++" "-static-libgcc")
    endif()
    add_definitions("-fPIC")
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
endif()
if (MINGW)
    list(APPEND VST_LIBS "-lstdc++fs")
    if (STATIC_LIBS)
        list(APPEND VST_LIBS "-static-libstdc++" "-static-libgcc" "-static -lpthread")
    else()
        list(APPEND VST_LIBS "-lpthread")
    endif()
endif()
if (APPLE)
    list(APPEND VST_LIBS "-framework Cocoa" "-lpthread")
endif()

set(VST "vst")
set(LIBS ${VST} ${VST_LIBS})
if (HOST32)
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

if (HOST32)
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
