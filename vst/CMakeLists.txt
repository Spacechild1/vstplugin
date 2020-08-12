cmake_minimum_required (VERSION 2.8)

set(SRC "Interface.h" "Utility.h" "PluginManager.h" "ThreadedPlugin.h"
    "PluginInfo.cpp" "PluginManager.cpp" "ThreadedPlugin.cpp"
    "CpuArch.cpp" "PluginFactory.h" "PluginFactory.cpp" "Search.cpp"
    "Module.cpp" "Utility.cpp" "Sync.cpp")

# VST2 SDK
if (VST2)
    include_directories(${VST2DIR}/pluginterfaces/vst2.x)
    list(APPEND SRC "VST2Plugin.h" "VST2Plugin.cpp")
endif()

# VST3 SDK
if (VST3)
    include_directories(${VST3DIR})
    include_directories(${VST3DIR}/pluginterfaces)
    include_directories(${VST3DIR}/pluginterfaces/base)
    include_directories(${VST3DIR}/pluginterfaces/vst)
    include_directories(${VST3DIR}/pluginterfaces/gui)
    list(APPEND SRC "VST3Plugin.h" "VST3Plugin.cpp")
endif()

# bit bridging
if (BRIDGE)
    list(APPEND SRC "PluginBridge.h" "PluginBridge.cpp"
        "PluginClient.h" "PluginClient.cpp"
        "PluginServer.h" "PluginServer.cpp"
        "ShmInterface.h" "ShmInterface.cpp")
endif()

# platform specific VST sources and defines
if (LINUX)
    add_definitions(-DTARGET_API_MAC_CARBON=1 -DDL_OPEN=1 -DUSE_X11=1)
    list(APPEND SRC "WindowX11.h" "WindowX11.cpp")
endif()
if (MINGW OR WIN32)
    list(APPEND SRC "WindowWin32.h" "WindowWin32.cpp")
endif()
if (APPLE)
    add_definitions(-fno-objc-arc)
    list(APPEND SRC "WindowCocoa.h" "WindowCocoa.mm")
endif()

# static library

add_library(${VST} STATIC ${SRC})
if (BUILD_HOST32)
    add_library(${VST32} STATIC ${SRC})
    target_compile_options(${VST32} PUBLIC "-m32")
endif()
