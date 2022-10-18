set(SRC "Bus.h" "CpuArch.cpp" "CpuArch.h" "FileUtils.cpp" "FileUtils.h" "Interface.h"
    "HostApp.cpp" "HostApp.h" "Lockfree.h" "Log.h" "MiscUtils.cpp" "MiscUtils.h" "Module.cpp"
    "PluginCommand.h" "PluginDesc.cpp" "PluginDesc.h" "PluginDictionary.cpp" "PluginDictionary.h"
    "PluginFactory.cpp" "PluginFactory.h" "Search.cpp" "Sync.cpp" "Sync.h"
    "ThreadedPlugin.cpp" "ThreadedPlugin.h"
    )

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
    # if VST2 SDK is included in VST3 SDK
    include_directories(${VST3DIR}/pluginterfaces/vst2.x)
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
if (WIN32 OR BUILD_WINE)
    list(APPEND SRC "WindowWin32.h" "WindowWin32.cpp")
elseif (LINUX)
    add_definitions(-DTARGET_API_MAC_CARBON=1)
    list(APPEND SRC "WindowX11.h" "WindowX11.cpp")
elseif (APPLE)
    add_compile_options("-fno-objc-arc")
    list(APPEND SRC "WindowCocoa.h" "WindowCocoa.mm")
endif()

# static library

if (LIBVST)
    add_library(${LIBVST} STATIC ${SRC})
endif()
if (LIBVST32)
    add_library(${LIBVST32} STATIC ${SRC})
    target_compile_options(${LIBVST32} PUBLIC "-m32")
endif()
if (LIBVST_AMD64)
    add_library(${LIBVST_AMD64} STATIC ${SRC})
    set_target_properties(${LIBVST_AMD64} PROPERTIES OSX_ARCHITECTURES "x86_64")
endif()
