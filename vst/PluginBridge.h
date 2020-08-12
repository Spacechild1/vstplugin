#pragma once

#include "ShmInterface.h"

#include <memory>

namespace vst {

class PluginInfo;
enum class CpuArch;

class PluginBridge final {
 public:
    using ptr = std::shared_ptr<PluginBridge>;

    static PluginBridge::ptr getShared(CpuArch arch);
    static PluginBridge::ptr create(CpuArch arch, const PluginInfo& desc);

    PluginBridge(CpuArch arch, const PluginInfo* desc);
    PluginBridge(const PluginBridge&) = delete;
    ~PluginBridge();
 private:
    ShmInterface shm_;
};

} // vst
