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

set(FILENAME "vstplugin~.cpp")
set(PROBE "probe")
set(TOP "${CMAKE_SOURCE_DIR}/..")
set(PD "${CMAKE_SOURCE_DIR}/pd")
set(VST "${TOP}/vst")

# Pd paths:
if (WIN32 OR MINGW)
    # check if "Program Files (x86)" exists (64-bit Windows) and if we compile for 32-bit
    set(_pf_x86 "ProgramFiles(x86)")
    if (DEFINED ENV{${_pf_x86}} AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(PROGRAMFILES $ENV{${_pf_x86}})
    else()
        set(PROGRAMFILES $ENV{PROGRAMFILES})
    endif()
endif()

if (WIN32 OR MINGW)
    set(PDINCLUDEDIR "${PROGRAMFILES}/Pd/src" CACHE PATH "Pd header files")
elseif(APPLE)
    # isn't there a better way?
    file(GLOB _globresults "/Applications/Pd*.app/Contents/Resources/src")
    list(GET _globresults 0 _globresult)
    set(PDINCLUDEDIR ${_globresult} CACHE PATH "Pd header files")
else()
    set(PDINCLUDEDIR "/usr/include/pd" CACHE PATH "Pd header files")
endif()
include_directories("${PDINCLUDEDIR}")
message(STATUS "PDINCLUDEDIR: ${PDINCLUDEDIR}")

if (WIN32 OR MINGW)
    set(PDBINDIR "${PROGRAMFILES}/Pd/bin" CACHE PATH "Pd binaries")
    message(STATUS "PDBINDIR: ${PDBINDIR}")
endif()

if (WIN32 OR MINGW)
    set(PDLIBDIR "$ENV{APPDATA}/Pd" CACHE PATH "Installation path")
elseif(APPLE)
    set(PDLIBDIR "~/Library/Pd" CACHE PATH "Installation path")
else()
    set(PDLIBDIR "/usr/local/lib/pd-externals" CACHE PATH "Installation path")
endif()
message(STATUS "PDLIBDIR: ${PDLIBDIR}")

SET(CMAKE_INSTALL_PREFIX "${PDLIBDIR}" CACHE INTERNAL "Prefix
    prepended to install directories" FORCE)

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

set(CMAKE_SHARED_MODULE_PREFIX "")
if (NOT EXTENSION)
    if (WIN32 OR MINGW)
        set(CMAKE_SHARED_MODULE_SUFFIX ".dll")
    elseif(APPLE)
        set(CMAKE_SHARED_MODULE_SUFFIX ".pd_darwin")
    elseif(LINUX)
        set(CMAKE_SHARED_MODULE_SUFFIX ".pd_linux")
    endif()
else()
    set(CMAKE_SHARED_MODULE_SUFFIX ${EXTENSION})
endif()
message(STATUS "EXTENSION: ${CMAKE_SHARED_MODULE_SUFFIX}")

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
    add_definitions(-fno-objc-arc -DHAVE_UI_THREAD=0)
    list(APPEND VST_SRC "${VST}/WindowCocoa.mm")
    list(APPEND VST_LIBS "-framework Cocoa")
endif()

target_sources(${PROJECT} PUBLIC ${VST_SRC})
target_sources(${PROBE} PUBLIC ${VST_SRC})
target_link_libraries(${PROJECT} ${VST_LIBS})
if (WIN32 OR MINGW)
    target_link_libraries(${PROJECT} "${PDBINDIR}/pd.lib")
elseif(APPLE)
    set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS "-undefined suppress -flat_namespace -bundle")
else()
    set_target_properties(${PROJECT} PROPERTIES COMPILE_FLAGS "-fPIC -fcheck-new")
    set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS "-rdynamic -fPIC")
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
endif()
target_link_libraries(${PROBE} ${VST_LIBS})

install(TARGETS ${PROJECT} ${PROBE} DESTINATION ${PROJECT})
install(FILES "${PROJECT}-help.pd" "vstpluginbrowser.pd" "vstpluginbrowser-help.pd"
    "${TOP}/README.md" "${TOP}/LICENSE.txt" DESTINATION ${PROJECT})
