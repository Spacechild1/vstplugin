set(FILENAME "VSTPluginUGen.cpp") #specify the .cpp file here
cmake_minimum_required (VERSION 2.8)
# get_filename_component(PROJECT ${FILENAME} NAME_WE) #automatically sets project name from the filename
set(PROJECT "VSTPlugin") #alternatively set project name manually
message(STATUS "Project name is ${PROJECT}")
project (${PROJECT})

message(STATUS "Installation path is ${CMAKE_INSTALL_PREFIX}")
set(PROBE "probe")

if(UNIX AND NOT APPLE AND NOT MINGW)
set(LINUX TRUE)
endif()

# SuperCollider API:
set(SC_PATH "" CACHE PATH "SuperCollider source directory")
message(STATUS "SC_PATH is ${SC_PATH}")
include_directories(${SC_PATH}/include/plugin_interface)
include_directories(${SC_PATH}/include/common)
include_directories(${SC_PATH}/plugin_interface)
include_directories(${SC_PATH}/common)

# VST
set(VST_SRC "../vst/VSTPlugin.cpp")
set(VST_LIBS)

# VST2 SDK:
option(VST2 "Enable VST2.x plug-ins" ON)
if (VST2)
    add_definitions(-DUSE_VST2=1)
    list(APPEND VST_SRC "../vst/VST2Plugin.cpp")
    set(VST2DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vst/VST_SDK/VST2_SDK/" CACHE PATH "path to VST2_SDK")
    if(IS_DIRECTORY ${VST2DIR})
      message(STATUS "VST2DIR is ${VST2DIR}")
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
    list(APPEND VST_SRC "../vst/VST3Plugin.cpp")
    set(VST3DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vst/VST_SDK/VST3_SDK/" CACHE PATH "path to VST3_SDK")
    include_directories(${VST3DIR})
    include_directories(${VST3DIR}/pluginterfaces)
    include_directories(${VST3DIR}/pluginterfaces/base)
    include_directories(${VST3DIR}/pluginterfaces/vst)
    include_directories(${VST3DIR}/pluginterfaces/gui)
endif()

include_directories(src)
include_directories(../vst)

set(LOGLEVEL 2 CACHE STRING "LOGLEVEL")

set(CMAKE_SHARED_MODULE_PREFIX "")
if(APPLE OR MINGW OR WIN32)
set(CMAKE_SHARED_MODULE_SUFFIX ".scx")
endif()

option(SUPERNOVA "Build plugins for supernova" OFF)
if (SUPERNOVA)
    include_directories(${SC_PATH}/external_libraries/nova-tt)
    include_directories(${SC_PATH}/external_libraries/nova-tt/nova-tt)
    # actually just boost.atomic
    include_directories(${SC_PATH}/external_libraries/boost)
    include_directories(${SC_PATH}/external_libraries/boost/lockfree)
    include_directories(${SC_PATH}/external_libraries/boost_lockfree)
    include_directories(${SC_PATH}/external_libraries/boost-lockfree)
endif()

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

add_library(${PROJECT} MODULE src/${FILENAME})
add_executable(${PROBE} ../vst/probe.cpp)
add_definitions(-DLOGLEVEL=${LOGLEVEL})
target_compile_definitions(${PROJECT} PUBLIC -DLOGFUNCTION=SCLog) # only for VSTPluginUGen!

if (LINUX)
    add_definitions(-DTARGET_API_MAC_CARBON=1 -DDL_OPEN=1 -DUSE_X11=1 -DVSTTHREADS=1)
    list(APPEND VST_SRC "../vst/VSTWindowX11.cpp")
    list(APPEND VST_LIBS "-ldl" "-L/usr/X11R6/lib" "-lX11")
endif()
if (MINGW OR WIN32)
    add_definitions(-DVSTTHREADS=1)
    list(APPEND VST_SRC "../vst/VSTWindowWin32.cpp")
endif()
if (MINGW)
    list(APPEND VST_LIBS "-static-libgcc" "-static-libstdc++" "-lstdc++fs")
    # -municode for wmain and -mwindows to hide console window
    target_link_libraries(${PROBE} -municode -mwindows)
endif()
if (APPLE)
    add_definitions(-fno-objc-arc -DVSTTHREADS=0)
    list(APPEND VST_SRC "../vst/VSTWindowCocoa.mm")
    list(APPEND VST_LIBS "-framework Cocoa")
endif()

target_sources(${PROJECT} PUBLIC ${VST_SRC})
target_sources(${PROBE} PUBLIC ${VST_SRC})
message(STATUS ${VST_LIBS})
target_link_libraries(${PROJECT} ${VST_LIBS})
target_link_libraries(${PROBE} ${VST_LIBS})

install(TARGETS ${PROJECT} ${PROBE} DESTINATION ${PROJECT}/plugins)
install(FILES VSTPlugin.sc VSTPluginController.sc VSTPluginGui.sc DESTINATION ${PROJECT}/classes)
install(FILES ../README.md ../LICENSE.txt DESTINATION ${PROJECT})
install(DIRECTORY HelpSource DESTINATION ${PROJECT})

if(SUPERNOVA)
    add_library(${PROJECT}_supernova MODULE src/${FILENAME} ${VST_SRC})

    target_compile_definitions(${PROJECT}_supernova PUBLIC -DSUPERNOVA=1 -DLOGFUNCTION=SCLog)
    target_sources(${PROJECT}_supernova PUBLIC ${VST_SRC})
    target_link_libraries(${PROJECT}_supernova ${VST_LIBS})

    install(TARGETS ${PROJECT}_supernova DESTINATION ${PROJECT}/plugins)
endif()
