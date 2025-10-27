# common public properties
add_library(vst_public INTERFACE)

# common private properties
add_library(vst_common INTERFACE)

target_include_directories(vst_public INTERFACE ".")

# logging
set(LOG_LEVEL "INFO" CACHE STRING "Log level")
set_property(CACHE LOG_LEVEL PROPERTY STRINGS "SILENT;ERROR;WARNING;INFO;DEBUG;VERBOSE")
message(STATUS "Log level: ${LOG_LEVEL}")
target_compile_definitions(vst_public INTERFACE LOG_LEVEL=LOG_LEVEL_${LOG_LEVEL})
# warn about old LOGLEVEL variable and remove it from the cache
if (DEFINED LOGLEVEL)
    message(WARNING "The LOGLEVEL option has been removed, please use LOG_LEVEL instead. "
            "Note that the log level is now provided as a string, e.g. 'INFO'")
    unset(LOGLEVEL CACHE)
endif()

# settings
if (LINUX)
    option(USE_RTLD_DEEPBIND "Load plugin modules with RTLD_DEEPBIND flag" OFF)
    if (USE_RTLD_DEEPBIND)
        message(STATUS "Load plugin modules with RTLD_DEEPBIND flag.")
    endif()
    target_compile_definitions(vst_common INTERFACE USE_RTLD_DEEPBIND=$<BOOL:${USE_RTLD_DEEPBIND}>)
endif()

# Windows version
if (WIN32)
    # requires at least Windows 7
    set(WINVER "0x0601" CACHE STRING "Minimum supported windows version")
    if (WINVER)
        target_compile_definitions(vst_public INTERFACE _WIN32_WINNT=${WINVER})
    endif()
endif()

target_sources(vst_common INTERFACE
    "Bus.h" "CpuArch.cpp" "CpuArch.h"
    "FileUtils.cpp" "FileUtils.h"
    "HostApp.cpp" "HostApp.h"
    "Interface.h" "Lockfree.h" "Log.h"
    "MiscUtils.cpp" "MiscUtils.h" "Module.cpp"
    "PluginCommand.h" "PluginDesc.cpp" "PluginDesc.h"
    "PluginDictionary.cpp" "PluginDictionary.h"
    "PluginFactory.cpp" "PluginFactory.h"
    "Search.cpp" "Sync.cpp" "Sync.h"
    "ThreadedPlugin.cpp" "ThreadedPlugin.h")

# platform specific VST sources and defines
if (WIN32 OR BUILD_WINE)
    target_sources(vst_common INTERFACE "WindowWin32.h" "WindowWin32.cpp")
elseif (LINUX)
    target_compile_definitions(vst_common INTERFACE TARGET_API_MAC_CARBON=1)
    target_sources(vst_common INTERFACE "WindowX11.h" "WindowX11.cpp")
elseif (APPLE)
    target_compile_options(vst_common INTERFACE -fno-objc-arc)
    target_sources(vst_common INTERFACE "WindowCocoa.h" "WindowCocoa.mm")
endif()

option(PROBE_LOG "show output of probe process" OFF)
mark_as_advanced(PROBE_LOG)
if (PROBE_LOG)
    target_compile_definitions(vst_common INTERFACE PROBE_LOG=1)
endif()

option(BRIDGE_LOG "show stdout/stderr of bridge process" OFF)
mark_as_advanced(BRIDGE_LOG)
if (BRIDGE_LOG)
    target_compile_definitions(vst_common INTERFACE BRIDGE_LOG=1)
endif()

if (BUILD_WINE)
    if (LINUX)
        target_compile_definitions(vst_public INTERFACE
            VST_HOST_SYSTEM=VST_LINUX)
    endif()

    if (APPLE)
        target_compile_definitions(vst_public INTERFACE
            VST_HOST_SYSTEM=VST_MACOS)
    endif()

    option(WINE_MSVCRT "Use Wine MSVCRT.dll instead of libc" OFF)
    if (WINE_MSVCRT)
        target_compile_definitions(vst_public INTERFACE -mno-cygwin)
    endif()
endif()

# use wmain?
if (WIN32 OR BUILD_WINE)
    if (WIN32)
        set(WMAIN ON CACHE BOOL "Use wmain")
    else()
        # wmain gives me trouble on Wine
        # (undefined reference to wWinMain)
        set(WMAIN OFF CACHE BOOL "Use wmain")
    endif()
    target_compile_definitions(vst_public INTERFACE
        USE_WMAIN=$<BOOL:${WMAIN}>)
endif()

# use std::filesystem?
if (WIN32)
    # always use on Windows!
    set(STDFS ON)
else()
    if (LINUX)
        if (BUILD_WINE)
            # std::filesystem causes big trouble on Wine...
            set(STDFS OFF CACHE BOOL "Use std::filesystem" FORCE)
        else()
            set(STDFS ON CACHE BOOL "Use std::filesystem")
        endif()
    else()
        # not available on older clang version on macOS
        set(STDFS OFF CACHE BOOL "Use std::filesystem")
    endif()
endif()
target_compile_definitions(vst_common INTERFACE USE_STDFS=$<BOOL:${STDFS}>)
if (STDFS)
    # for GCC 9.1> and Clang 9.0>
    if (CMAKE_COMPILER_IS_GNUCXX
            AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1))
        target_link_libraries(vst_common INTERFACE -lstdc++fs)
    elseif (CMAKE_COMPILER_IS_CLANG
            AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0))
        target_link_libraries(vst_common INTERFACE -lc++fs)
    endif()
endif()

# VST2 SDK
option(VST2 "Enable VST2.x plug-ins" ON)
target_compile_definitions(vst_public INTERFACE USE_VST2=$<BOOL:${VST2}>)
if (VST2)
    set(VST2DIR "${CMAKE_CURRENT_SOURCE_DIR}/VST_SDK/VST2_SDK/" CACHE PATH "path to VST2_SDK")
    message(STATUS "Build with VST2 support")
    message(STATUS "VST2DIR: ${VST2DIR}")
    target_include_directories(vst_common INTERFACE ${VST2DIR}/pluginterfaces/vst2.x)
    target_sources(vst_common INTERFACE "VST2Plugin.h" "VST2Plugin.cpp")
endif()

# VST3 SDK
option(VST3 "Enable VST3 plug-ins" ON)
target_compile_definitions(vst_public INTERFACE USE_VST3=$<BOOL:${VST3}>)
if (VST3)
    set(VST3DIR "${CMAKE_CURRENT_SOURCE_DIR}/VST_SDK/VST3_SDK/" CACHE PATH "path to VST3_SDK")
    message(STATUS "Build with VST3 support")
    message(STATUS "VST3DIR: ${VST3DIR}")
    target_include_directories(vst_common INTERFACE
        ${VST3DIR}
        ${VST3DIR}/pluginterfaces
        ${VST3DIR}/pluginterfaces/base
        ${VST3DIR}/pluginterfaces/vst
        ${VST3DIR}/pluginterfaces/gui
        # if VST2 SDK is included in VST3 SDK
        ${VST3DIR}/pluginterfaces/vst2.x)
    target_sources(vst_common INTERFACE "VST3Plugin.h" "VST3Plugin.cpp")
endif()

# bit bridging
option(BRIDGE "Enable plugin bridge" ON)
target_compile_definitions(vst_public INTERFACE USE_BRIDGE=$<BOOL:${BRIDGE}>)
if (BRIDGE)
    message(STATUS "Enable plugin bridge")

    # build host apps
    if (LINUX)
        # On Linux/Wine we can always build a 32-bit host app if the required 32-bit
        # libraries are installed, so we always show the option. If we did a compile check
        # and the libraries were not installed (yet), the option would remain hidden...
        option(BUILD_HOST32 "Build 32-bit host app" OFF)
    endif()

    if (APPLE)
        # On macOS we only show the option if we do NOT build a universal binary.
        # (A universal host app already supports bridging via the 'arch' command, see vst/HostApp.cpp)
        list(LENGTH CMAKE_OSX_ARCHITECTURES numarchs)
        if (numarchs LESS 2)
            # Only show the 32-bit Intel option if supported by the toolchain.
            if (HAS_M32_SUPPORT AND NOT (CMAKE_OSX_ARCHITECTURES STREQUAL "i386"))
                option(BUILD_HOST32 "Build 32-bit host app" OFF)
            endif()
            # Only show the 64-bit Intel option if we are building for arm64 (only).
            if ((CMAKE_OSX_ARCHITECTURES STREQUAL "arm64") OR
                    (NOT CMAKE_OSX_ARCHITECTURES AND (CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")))
                option(BUILD_HOST_AMD64 "Build 64-bit Intel host app" OFF)
            endif()
        endif()
    endif()

    if (WIN32)
        # simplify host app installation
        if (CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(HOST_AMD64_PATH "" CACHE FILEPATH "(optional) absolute file path of the 64-bit host.exe")
        else()
            set(HOST32_PATH "" CACHE FILEPATH "(optional) absolute file path of the 32-bit host.exe")
        endif()
    endif()

    target_sources(vst_common INTERFACE
        "PluginBridge.h" "PluginBridge.cpp"
        "PluginClient.h" "PluginClient.cpp"
        "PluginServer.h" "PluginServer.cpp"
        "ShmInterface.h" "ShmInterface.cpp")
endif()

# Wine support on Linux
if (LINUX)
    option(WINE "Enable support for running Windows VSTs with Wine" ON)
    if (WINE)
        if (BRIDGE)
            message(STATUS "Enable Wine support")
        else()
            message(WARNING "Wine support requires BRIDGE=ON")
        endif()
    endif()
    target_compile_definitions(vst_public INTERFACE USE_WINE=$<BOOL:${WINE}>)
endif()

# static linking?
if(LINUX AND CMAKE_COMPILER_IS_GNUCXX)
    option(STATIC_LIBS "link with static libraries (libstdc++ and libgcc)" OFF)
endif()

if(MINGW)
    option(STATIC_LIBS "link with static libraries (libstdc++, libgcc and phread)" ON)
endif()

# platform specific linker flags
if (LINUX)
    if (BRIDGE)
        target_link_libraries(vst_common INTERFACE -lrt)
    endif()
    if (BUILD_WINE)
        target_link_libraries(vst_common INTERFACE -lpthread)
    else()
        target_compile_options(vst_common INTERFACE -pthread)
        target_link_libraries(vst_common INTERFACE -pthread -ldl -L/usr/X11R6/lib -lX11)
    endif()
    if(STATIC_LIBS)
        target_link_libraries(vst_common INTERFACE -static-libstdc++ -static-libgcc)
        message(STATUS "Linking statically with libstdc++ and libgcc")
    endif()
    target_compile_options(vst_common INTERFACE -fPIC)
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
endif()

if (WIN32 OR BUILD_WINE)
    # for COM
    target_link_libraries(vst_common INTERFACE ole32)
endif()

if (BUILD_WINE)
    # HACK: --strip resp. install/strip only works for binaries installed as targets,
    # but we need to install the wine host(s) as programs. As a workaround, we manually
    # strip the symbols at link time - but only in Release mode.
    add_link_options($<$<CONFIG:RELEASE>:-s>)
endif()

if (MINGW)
    if (STATIC_LIBS)
        target_link_libraries(vst_common INTERFACE -static-libstdc++ -static-libgcc "-static -lpthread")
        message(STATUS "Linking statically with libstdc++, libgcc and libpthread")
    else()
        target_link_libraries(vst_common INTERFACE -lpthread)
    endif()
endif()

if (APPLE)
    target_link_libraries(vst_common INTERFACE "-framework Cocoa" -lpthread)
endif()

# static libraries
add_library(vst STATIC)
target_link_libraries(vst PUBLIC vst_public PRIVATE vst_common)

if (BUILD_HOST32)
    add_library(vst32 STATIC)
    target_link_libraries(vst32 PUBLIC vst_public PRIVATE vst_common)
    target_compile_options(vst32 PUBLIC -m32)
endif()

if (BUILD_HOST_AMD64)
    add_library(vst_amd64 STATIC)
    target_link_libraries(vst_amd64 PUBLIC vst_public PRIVATE vst_common)
    set_target_properties(vst_amd64 PROPERTIES OSX_ARCHITECTURES x86_64)
endif()

# host
add_executable(host "host.cpp") # HOST exe
if (BUILD_WINE)
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(HOSTNAME "host_pe_amd64")
    elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(HOSTNAME "host_pe_i386")
    else()
        message(FATAL_ERROR "unexpected value for CMAKE_SIZEOF_VOID_P")
    endif()
    set_target_properties(host PROPERTIES OUTPUT_NAME "${HOSTNAME}")
endif()
target_link_libraries(host vst)
if (WMAIN AND (MINGW OR BUILD_WINE))
    # -municode must be a linker flag!
    target_link_libraries(host -municode)
endif()

# 32-bit host
if (BUILD_HOST32)
    message(STATUS "Build 32-bit host program")
    # is -m32 always Intel?
    add_executable(host32 "host.cpp")
    target_compile_options(host32 PUBLIC -m32)
    target_link_options(host32 PUBLIC -m32)
    target_link_libraries(host32 vst32)
    if (WMAIN AND (MINGW OR BUILD_WINE))
        # -municode must be a linker flag!
        target_link_libraries(host32 -municode)
    endif()
    if (BUILD_WINE)
        set(HOSTNAME32 "host_pe_i386")
    else()
        set(HOSTNAME32 "host_i386")
    endif()
    set_target_properties(host32 PROPERTIES OUTPUT_NAME "${HOSTNAME32}")
endif()

# 64-bit Intel host (for Apple M1)
if (BUILD_HOST_AMD64)
    message(STATUS "Build 64-bit Intel host program")
    set(HOST_AMD64 "host_amd64")
    add_executable(host_amd64 "host.cpp")
    set_target_properties(host_amd64 PROPERTIES OSX_ARCHITECTURES "x86_64")
    target_link_libraries(host_amd64 vst_amd64)
endif()
