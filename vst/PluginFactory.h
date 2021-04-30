#pragma once

#include "Interface.h"
#include "PluginInfo.h"
#include "CpuArch.h"

// for testing we don't want to load hundreds of sub plugins
// #define PLUGIN_LIMIT 50

// We probe sub-plugins asynchronously with "futures".
// Each future spawns a subprocess and then waits for the results.
#define PROBE_FUTURES 8 // max. number of futures to wait for

// The sleep interval when probing several plugins in a factory asynchronously
#define PROBE_SLEEP_MS 2

namespace vst {

class PluginFactory :
        public std::enable_shared_from_this<PluginFactory>,
        public IFactory
{
 public:
    PluginFactory(const std::string& path);
    virtual ~PluginFactory(){}

    ProbeFuture probeAsync(float timeout, bool nonblocking) override;

    void addPlugin(PluginInfo::ptr desc) override;
    PluginInfo::const_ptr getPlugin(int index) const override;
    PluginInfo::const_ptr findPlugin(const std::string& name) const override;
    int numPlugins() const override;

    const std::string& path() const override { return path_; }
    CpuArch arch() const override { return arch_; }
 protected:
    using ProbeResultFuture = std::function<bool(ProbeResult&)>;
    ProbeResultFuture doProbePlugin(float timeout, bool nonblocking);
    ProbeResultFuture doProbePlugin(const PluginInfo::SubPlugin& subplugin,
                                    float timeout, bool nonblocking);
    std::vector<PluginInfo::ptr> doProbePlugins(
            const PluginInfo::SubPluginList& pluginList,
            float timeout, ProbeCallback callback);
    // data
    std::string path_;
    CpuArch arch_;
    std::unique_ptr<IModule> module_;
    std::vector<PluginInfo::ptr> plugins_;
    std::unordered_map<std::string, PluginInfo::ptr> pluginMap_;
};

} // vst
