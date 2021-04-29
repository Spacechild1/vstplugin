#pragma once

#include "Interface.h"
#include "PluginInfo.h"
#include "CpuArch.h"

// probe timeout in seconds
// 0: infinite
//
// we choose a rather large timeout to eliminate
// false positives. The Pd/SC clients will periodically
// print a warning that we're still waiting for plugins, so the
// user can also force quit and remove the offending plugin(s).
#ifndef PROBE_TIMEOUT
# define PROBE_TIMEOUT 60
#endif

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
