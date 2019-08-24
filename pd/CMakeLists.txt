cmake_minimum_required (VERSION 2.8)

set(PROJECT "vstplugin~")
message(STATUS "Project name: ${PROJECT}")
project (${PROJECT})

if(UNIX AND NOT APPLE AND NOT MINGW)
	set(LINUX TRUE)
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

if(MINGW)
	option(STATIC_LIBS "link with static libraries (libstdc++, libgcc and phread)" ON)
endif()

message(STATUS "Installation path: ${CMAKE_INSTALL_PREFIX}")

set(FILENAME "vstplugin~.cpp")
set(PROBE "probe")
set(TOP "${CMAKE_SOURCE_DIR}/..")
set(PD "${CMAKE_SOURCE_DIR}/pd")
set(VST "${TOP}/vst")

# Pd API:
set(PD_PATH "" CACHE PATH "Pd installation directory")
message(STATUS "PD_PATH: ${PD_PATH}")
include_directories(${PD_PATH}/src)

# VST
set(VST_SRC "${VST}/Plugin.cpp")
set(VST_LIBS)

# VST2 SDK:
option(VST2 "Enable VST2.x plug-ins" ON)
if (VST2)
    set(VST2DIR "${VST}/VST_SDK/VST2_SDK/" CACHE PATH "path to VST2_SDK")
    add_definitions(-DUSE_VST2=1)
    list(APPEND VST_SRC "${VST}/VST2Plugin.cpp")
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
option(VST3 "Enable VST3 plug-ins" ON)
if (VST3)
    set(VST3DIR "${VST}/VST_SDK/VST3_SDK/" CACHE PATH "path to VST3_SDK")
    add_definitions(-DUSE_VST3=1)
    list(APPEND VST_SRC "${VST}/VST3Plugin.cpp")
    include_directories(${VST3DIR})
    include_directories(${VST3DIR}/pluginterfaces)
    include_directories(${VST3DIR}/pluginterfaces/base)
    include_directories(${VST3DIR}/pluginterfaces/vst)
    include_directories(${VST3DIR}/pluginterfaces/gui)
    message(STATUS "VST3DIR: ${VST3DIR}")
endif()

include_directories(src)
include_directories(${VST})

set(LOGLEVEL 2 CACHE STRING "LOGLEVEL")
message(STATUS "LOGLEVEL: ${LOGLEVEL}")

if (WIN32 OR MINGW)
    set(CMAKE_SHARED_MODULE_SUFFIX ".dll")
elseif(APPLE)
    set(CMAKE_SHARED_MODULE_SUFFIX ".pd_darwin")
elseif(LINUX)
    set(CMAKE_SHARED_MODULE_SUFFIX ".pd_linux")
endif()

if (MINGW)
    set(CMAKE_EXECUTABLE_SUFFIX ".exe")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_COMPILER_IS_CLANG 1)
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

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -ffast-math -funroll-loops -fomit-frame-pointer")

    if(CMAKE_COMPILER_IS_CLANG)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
    endif()
endif()

add_library(${PROJECT} MODULE src/${FILENAME}) # the Pd external
add_executable(${PROBE} "${VST}/${PROBE}.cpp") # probe exe

# logging
add_definitions(-DLOGLEVEL=${LOGLEVEL})

if (LINUX)
    add_definitions(-DTARGET_API_MAC_CARBON=1 -DDL_OPEN=1 -DUSE_X11=1 -DHAVE_UI_THREAD=1)
    list(APPEND VST_SRC "${VST}/WindowX11.cpp")
    list(APPEND VST_LIBS "-ldl" "-L/usr/X11R6/lib" "-lX11" "-pthread")
endif()
if (MINGW OR WIN32)
    add_definitions(-DHAVE_UI_THREAD=1)
    list(APPEND VST_SRC "${VST}/WindowWin32.cpp")
endif()
if (MINGW)
    list(APPEND VST_LIBS "-lstdc++fs")
    if (STATIC_LIBS)
        list(APPEND VST_LIBS "-static-libgcc" "-static-libstdc++" "-static -lpthread")
    else()
        list(APPEND VST_LIBS "-lpthread")
    endif()
    # -municode for wmain and -mwindows to hide console window
    target_link_libraries(${PROBE} -municode -mwindows)
endif()
if (APPLE)
    add_definitions(-fno-objc-arc -DHAVE_UI_THREAD=1)
    list(APPEND VST_SRC "${VST}/WindowCocoa.mm")
    list(APPEND VST_LIBS "-framework Cocoa")
endif()

target_sources(${PROJECT} PUBLIC ${VST_SRC})
target_sources(${PROBE} PUBLIC ${VST_SRC})
target_link_libraries(${PROJECT} ${VST_LIBS})
if (WIN32 OR MINGW)
    target_link_libraries(${PROJECT} "${PD_PATH}/bin/pd.lib")
endif()
target_link_libraries(${PROBE} ${VST_LIBS})

install(TARGETS ${PROJECT} ${PROBE} DESTINATION ${PROJECT})
install(FILES "${PROJECT}-help.pd" "vstpluginbrowser.pd" "vstpluginbrowser-help.pd"
    "${TOP}/README.md" "${TOP}/LICENSE.txt" DESTINATION ${PROJECT})
