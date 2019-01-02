set(FILENAME "VstPluginUGen.cpp") #specify the .cpp file here
cmake_minimum_required (VERSION 2.8)
get_filename_component(PROJECT ${FILENAME} NAME_WE) #automatically sets project name from the filename
# set(PROJECT "my_name") #alternatively set project name manually
message(STATUS "Project name is ${PROJECT}")
project (${PROJECT})

if(UNIX AND NOT APPLE AND NOT MINGW)
set(LINUX TRUE)
endif()

# SuperCollider API:
message(STATUS "SC_PATH is ${SC_PATH}")
include_directories(${SC_PATH}/include/plugin_interface)
include_directories(${SC_PATH}/include/common)
include_directories(${SC_PATH}/plugin_interface)
include_directories(${SC_PATH}/common)

# VST2 SDK:
set(VST2DIR "src/VST_SDK/VST2_SDK/" CACHE PATH "path to VST2_SDK")
message(STATUS "VST2DIR is ${VST2DIR}")
include_directories(${VST2DIR}/pluginterfaces/vst2.x)

include_directories(src)

add_definitions(-DLOGLEVEL=3 -DLOGFUNCTION=SCLog)

set(CMAKE_SHARED_MODULE_PREFIX "")
if(APPLE OR MINGW OR WIN32)
set(CMAKE_SHARED_MODULE_SUFFIX ".scx")
endif()

option(SUPERNOVA "Build plugins for supernova" OFF)
if (SUPERNOVA)
    include_directories(${SC_PATH}/external_libraries/nova-tt)
    # actually just boost.atomic
    include_directories(${SC_PATH}/external_libraries/boost)
    include_directories(${SC_PATH}/external_libraries/boost_lockfree)
    include_directories(${SC_PATH}/external_libraries/boost-lockfree)
endif()

option(CPP14 "Build with c++14." ON)

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

    if(CPP14)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14")
        if(CMAKE_COMPILER_IS_CLANG)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
        endif()
    endif()
endif()
if(MINGW)
    set(CMAKE_C_COMPILER gcc)
    set(CMAKE_CXX_COMPILER g++)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mstackrealign")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mstackrealign")
endif()

add_library(${PROJECT} MODULE src/${FILENAME} src/VST2Plugin.cpp src/VSTPlugin.cpp)
if (LINUX)
    add_definitions(-DTARGET_API_MAC_CARBON=1 -DDL_OPEN=1 -DUSE_X11=1 -DVSTTHREADS=1)
    target_sources(${PROJECT} PUBLIC src/VSTWindowX11.cpp)
    target_link_libraries(${PROJECT} -ldl -L/usr/X11R6/lib -lX11)
endif()
if (MINGW OR WIN32)
    add_definitions(-DVSTTHREADS=1)
    target_sources(${PROJECT} PUBLIC src/VSTWindowWin32.cpp)
endif()
if (MINGW)
    target_link_libraries(${PROJECT} "-static-libgcc -static-libstdc++")
endif()
if (APPLE)
    add_definitions(-fno-objc-arc -DVSTTHREADS=0)
    target_sources(${PROJECT} PUBLIC src/VSTWindowCocoa.mm)
    target_link_libraries(${PROJECT} "-framework Cocoa")
endif()

if(SUPERNOVA)
    add_library(${PROJECT}_supernova MODULE ${FILENAME})
    set_property(TARGET ${PROJECT}_supernova
                 PROPERTY COMPILE_DEFINITIONS SUPERNOVA)
endif()
