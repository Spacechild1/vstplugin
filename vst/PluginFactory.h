#pragma once

#include "Interface.h"
#include "PluginInfo.h"
#include "Utility.h"

namespace vst {

class PluginFactory :
        public std::enable_shared_from_this<PluginFactory>,
        public IFactory
{
 public:
    PluginFactory(const std::string& path);
    virtual ~PluginFactory(){}

    ProbeFuture probeAsync(bool nonblocking) override;

    void addPlugin(PluginInfo::ptr desc) override;
    PluginInfo::const_ptr getPlugin(int index) const override;
    PluginInfo::const_ptr findPlugin(const std::string& name) const override;
    int numPlugins() const override;

    const std::string& path() const override { return path_; }
    CpuArch arch() const override { return arch_; }
 protected:
    using ProbeResultFuture = std::function<bool(ProbeResult&)>;
    ProbeResultFuture doProbePlugin(bool nonblocking);
    ProbeResultFuture doProbePlugin(const PluginInfo::SubPlugin& subplugin, bool nonblocking);
    std::vector<PluginInfo::ptr> doProbePlugins(
            const PluginInfo::SubPluginList& pluginList, ProbeCallback callback);
    // data
    std::string path_;
    CpuArch arch_;
    std::unique_ptr<IModule> module_;
    std::vector<PluginInfo::ptr> plugins_;
    std::unordered_map<std::string, PluginInfo::ptr> pluginMap_;
};

} // vst
