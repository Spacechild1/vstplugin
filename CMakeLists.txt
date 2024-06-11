cmake_minimum_required (VERSION 3.15)

# minimum macOS deployment target; must come before project()!
if (APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.9" CACHE STRING "Minimum OSX deployment version")
endif()

project(vstplugin)
message(STATUS "---\n*** general ***")

include (CheckCXXCompilerFlag)

if(UNIX AND NOT APPLE AND NOT MINGW)
    set(LINUX TRUE)
endif()

# some MinGW setups don't define WIN32!
if (MINGW AND NOT WIN32)
    message(WARNING "Your MinGW setup doesn't define WIN32")
    set(WIN32 TRUE)
endif()

# build Wine host
if (LINUX)
    option(BUILD_WINE "Build Wine host(s)" OFF)
    if (BUILD_WINE)
        message(STATUS "Build Wine host")
        # set globally!
        set(CMAKE_C_COMPILER winegcc)
        set(CMAKE_CXX_COMPILER wineg++)
    endif()
endif()

if (MINGW)
    set(CMAKE_EXECUTABLE_SUFFIX ".exe")
endif()

# check for Clang or AppleClang
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_COMPILER_IS_CLANG 1)
endif()

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# check if the compiler can produce 32-bit binaries.
# For some reason we must move it up here...
if (APPLE)
    CHECK_CXX_COMPILER_FLAG(-m32 HAS_M32_SUPPORT)
endif()
if (LINUX)
    # CHECK_CXX_COMPILER_FLAG(-m32) fails with a linker error...
    # For now assume that -m32 is supported. LATER fix this.
    set(HAS_M32_SUPPORT 1)
endif()

# windres
if (WIN32 AND MINGW)
    # set windres.exe options
    # for some reason, CMake does not automatically take the correct windres.exe;
    # instead, it just grabs the first one it can find. For example, it might use a
    # 64-bit windres.exe while building with mingw32, which would lead to linker errors.
    # As a workaround, we explicitly pass the desired target architectre.
    if (CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(CMAKE_RC_FLAGS --target=pe-i386 ${CMAKE_RC_FLAGS})
    else()
        set(CMAKE_RC_FLAGS --target=pe-x86-64 ${CMAKE_RC_FLAGS})
    endif()
    # message(STATUS "CMAKE_RC_COMPILER: ${CMAKE_RC_COMPILER}")
    # message(STATUS "CMAKE_RC_FLAGS: ${CMAKE_RC_FLAGS}")

    # The solution above does not always work (e.g. when using a 32-bit windres.exe with mingw64),
    # so we need a feature test.
    if (NOT DEFINED HAVE_WINDRES)
        set(RC_TEST_OPTIONS -O coff ${CMAKE_RC_FLAGS} -i "${CMAKE_CURRENT_SOURCE_DIR}/sc/src/VSTPlugin.rc"
            -o "${CMAKE_CURRENT_BINARY_DIR}/rc_test.obj")
        execute_process(COMMAND "${CMAKE_RC_COMPILER}" ${RC_TEST_OPTIONS} RESULT_VARIABLE RC_TEST_RESULT)
        if (RC_TEST_RESULT EQUAL 0)
            set(HAVE_WINDRES 1 CACHE INTERNAL "RC compiler test")
            message(STATUS "windres.exe works!")
        else()
            set(HAVE_WINDRES 0 CACHE INTERNAL "RC compiler test")
            message(WARNING "Wrong windres.exe! Try to explicitly set CMAKE_RC_COMPILER.")
        endif()
    endif()
endif()

# Windows paths
if (WIN32)
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

# global compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/Zc:__cplusplus /fp:fast)
endif()

if (CMAKE_COMPILER_IS_GNUCXX OR CMAKE_COMPILER_IS_CLANG)
    add_compile_options(-fvisibility=hidden)
    # disable some warnings
    add_compile_options(-Wno-unknown-pragmas -Wno-format-security)

    CHECK_CXX_COMPILER_FLAG(-msse HAS_CXX_SSE)
    if (HAS_CXX_SSE)
        add_compile_options(-msse)
    endif()

    CHECK_CXX_COMPILER_FLAG(-msse2 HAS_CXX_SSE2)
    if (HAS_CXX_SSE2)
        add_compile_options(-msse2)
    endif()

    CHECK_CXX_COMPILER_FLAG(-msse3 HAS_CXX_SSE3)
    if (HAS_CXX_SSE3)
        add_compile_options(-msse3)
    endif()

    # some people might still use old AMD CPUs...
    if (OFF)
        CHECK_CXX_COMPILER_FLAG(-msse4 HAS_CXX_SSE4)
        if (HAS_CXX_SSE4)
            add_compile_options(-msse4)
        endif()
    endif()

    CHECK_CXX_COMPILER_FLAG(-mfpmath=sse HAS_CXX_FPMATH_SSE)
    if (HAS_CXX_FPMATH_SSE)
        add_compile_options(-mfpmath=sse)
    endif()

    option(NATIVE "optimize for this machine (not portable!)" OFF)
    if (NATIVE)
        add_compile_options(-march=native)
    endif()

    add_compile_options(-ffast-math -funroll-loops -fomit-frame-pointer)
    add_compile_options(-Wstrict-aliasing)

    if (CMAKE_COMPILER_IS_CLANG)
        add_compile_options(-stdlib=libc++)
    endif()
endif()

if (MINGW)
    add_compile_options(-mstackrealign)
    # HACK: somehow --strip resp. install/strip target do not work on MSys2,
    # so we manually strip the symbols at link time - but only in Release mode.
    add_link_options($<$<CONFIG:RELEASE>:-s>)
endif()

# vst library
add_subdirectory(vst)

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

# test suite
option(TESTSUITE "build test suite" ON)
if (TESTSUITE)
    add_subdirectory(test)
endif()
