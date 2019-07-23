cmake_minimum_required (VERSION 2.8)

set(PROJECT "VSTPlugin")
message(STATUS "Project name: ${PROJECT}")
project (${PROJECT})

if(UNIX AND NOT APPLE AND NOT MINGW)
set(LINUX TRUE)
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

message(STATUS "Installation path: ${CMAKE_INSTALL_PREFIX}")

set(FILENAME "VSTPlugin.cpp")
set(PROBE "probe")
set(TOP "${CMAKE_SOURCE_DIR}/..")
set(SC "${CMAKE_SOURCE_DIR}/sc")
set(VST "${TOP}/vst")

# SuperCollider API:
set(SC_PATH "" CACHE PATH "SuperCollider source directory")
message(STATUS "SC_PATH: ${SC_PATH}")
include_directories(${SC_PATH}/include/plugin_interface)
include_directories(${SC_PATH}/include/common)
include_directories(${SC_PATH}/plugin_interface)
include_directories(${SC_PATH}/common)

# VST
set(VST_SRC "${VST}/Plugin.cpp")
set(VST_LIBS)

# VST2 SDK:
option(VST2 "Enable VST2.x plug-ins" ON)
if (VST2)
    add_definitions(-DUSE_VST2=1)
    list(APPEND VST_SRC "${VST}/VST2Plugin.cpp")
    set(VST2DIR "${VST}/VST_SDK/VST2_SDK/" CACHE PATH "path to VST2_SDK")
    if(IS_DIRECTORY ${VST2DIR})
      message(STATUS "VST2DIR: ${VST2DIR}")
      include_directories(${VST2DIR}/pluginterfaces/vst2.x)
    else()
      if(IS_DIRECTORY ${FST})
        message(STATUS "using FST in ${FST}")
        include_directories(${FST})
      else()
        message(STATUS "using FST")
      endif()
      add_definitions(-DUSE_FST=1)
    endif()
endif()

# VST3 SDK
option(VST3 "Enable VST3 plug-ins" OFF)
if (VST3)
    add_definitions(-DUSE_VST3=1)
    list(APPEND VST_SRC "${VST}/VST3Plugin.cpp")
    set(VST3DIR "${VST}/VST_SDK/VST3_SDK/" CACHE PATH "path to VST3_SDK")
    include_directories(${VST3DIR})
    include_directories(${VST3DIR}/pluginterfaces)
    include_directories(${VST3DIR}/pluginterfaces/base)
    include_directories(${VST3DIR}/pluginterfaces/vst)
    include_directories(${VST3DIR}/pluginterfaces/gui)
endif()

include_directories(src)
include_directories(${VST})

set(LOGLEVEL 2 CACHE STRING "LOGLEVEL")
message(STATUS "LOGLEVEL: ${LOGLEVEL}")

set(CMAKE_SHARED_MODULE_PREFIX "")
if(APPLE OR MINGW OR WIN32)
set(CMAKE_SHARED_MODULE_SUFFIX ".scx")
endif()

if (MINGW)
set(CMAKE_EXECUTABLE_SUFFIX ".exe")
endif()

option(SUPERNOVA "Build plugins for supernova" OFF)
if (SUPERNOVA)
	# for <nova-tt/spin_lock.h>
    include_directories(${SC_PATH}/external_libraries/nova-tt)
    # for <boost/atomic.hpp>
    include_directories(${SC_PATH}/external_libraries/boost)
endif()
message(STATUS "SUPERNOVA: ${SUPERNOVA}")

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_COMPILER_IS_CLANG 1)
endif()

if(CMAKE_COMPILER_IS_GNUCXX)
add_definitions(-Wl,--no-undefined)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_COMPILER_IS_CLANG)
    add_definitions(-fvisibility=hidden)

    include (CheckCCompilerFlag)
    include (CheckCXXCompilerFlag)

    CHECK_C_COMPILER_FLAG(-msse HAS_SSE)
    CHECK_CXX_COMPILER_FLAG(-msse HAS_CXX_SSE)

    if (HAS_SSE)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msse")
    endif()
    if (HAS_CXX_SSE)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse")
    endif()

    CHECK_C_COMPILER_FLAG(-msse2 HAS_SSE2)
    CHECK_CXX_COMPILER_FLAG(-msse2 HAS_CXX_SSE2)

    if (HAS_SSE2)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msse2")
    endif()
    if (HAS_CXX_SSE2)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse2")
    endif()

    CHECK_C_COMPILER_FLAG(-mfpmath=sse HAS_FPMATH_SSE)
    CHECK_CXX_COMPILER_FLAG(-mfpmath=sse HAS_CXX_FPMATH_SSE)

    if (HAS_FPMATH_SSE)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mfpmath=sse")
    endif()
    if (HAS_CXX_FPMATH_SSE)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mfpmath=sse")
    endif()

    if(NATIVE)
        add_definitions(-march=native)
    endif()

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14")
    if(CMAKE_COMPILER_IS_CLANG)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
    endif()
endif()
if(MINGW)
    set(CMAKE_C_COMPILER gcc)
    set(CMAKE_CXX_COMPILER g++)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mstackrealign")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mstackrealign")
endif()

add_library(${PROJECT} MODULE src/${FILENAME}) # the UGen
add_executable(${PROBE} ${VST}/probe.cpp ${VST_SRC}) # probe exe

add_definitions(-DLOGLEVEL=${LOGLEVEL})
target_compile_definitions(${PROJECT} PUBLIC -DLOGFUNCTION=SCLog) # only for VSTPluginUGen!

if (LINUX)
    add_definitions(-DTARGET_API_MAC_CARBON=1 -DDL_OPEN=1 -DUSE_X11=1 -DVSTTHREADS=1)
    list(APPEND VST_SRC "${VST}/WindowX11.cpp")
    list(APPEND VST_LIBS "-ldl" "-L/usr/X11R6/lib" "-lX11" "-pthread")
endif()
if (MINGW OR WIN32)
    add_definitions(-DVSTTHREADS=1)
    list(APPEND VST_SRC "${VST}/WindowWin32.cpp")
endif()
if (MINGW)
    list(APPEND VST_LIBS "-static-libgcc" "-static-libstdc++" "-lstdc++fs" "-static -lpthread")
    # -municode for wmain and -mwindows to hide console window
    target_link_libraries(${PROBE} -municode -mwindows)
endif()
if (APPLE)
    add_definitions(-fno-objc-arc -DVSTTHREADS=1)
    list(APPEND VST_SRC "${VST}/WindowCocoa.mm")
    list(APPEND VST_LIBS "-framework Cocoa")
endif()

target_sources(${PROJECT} PUBLIC ${VST_SRC})
target_sources(${PROBE} PUBLIC ${VST_SRC})
target_link_libraries(${PROJECT} ${VST_LIBS})
target_link_libraries(${PROBE} ${VST_LIBS})

install(TARGETS ${PROJECT} ${PROBE} DESTINATION ${PROJECT}/plugins)
install(DIRECTORY classes DESTINATION ${PROJECT})
install(DIRECTORY HelpSource DESTINATION ${PROJECT})
install(FILES "${TOP}/README.md" "${TOP}/LICENSE.txt" DESTINATION ${PROJECT})

if(SUPERNOVA)
    add_library(${PROJECT}_supernova MODULE src/${FILENAME} ${VST_SRC})

    target_compile_definitions(${PROJECT}_supernova PUBLIC -DSUPERNOVA=1 -DLOGFUNCTION=SCLog)
    target_sources(${PROJECT}_supernova PUBLIC ${VST_SRC})
    target_link_libraries(${PROJECT}_supernova ${VST_LIBS})

    install(TARGETS ${PROJECT}_supernova DESTINATION ${PROJECT}/plugins)
endif()
