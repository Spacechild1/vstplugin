#pragma once

#include "Interface.h"

namespace vst {

class PluginFactory :
        public std::enable_shared_from_this<PluginFactory>,
        public IFactory
{
 public:
    PluginFactory(const std::string& path)
        : path_(path){}
    virtual ~PluginFactory(){}

    void addPlugin(PluginInfo::ptr desc) override;
    PluginInfo::const_ptr getPlugin(int index) const override;
    PluginInfo::const_ptr findPlugin(const std::string& name) const override;
    int numPlugins() const override;

    const std::string& path() const override { return path_; }
 protected:
    using ProbeResultFuture = std::function<ProbeResult()>;
    ProbeResultFuture probePlugin(const std::string& name, int shellPluginID = 0);
    using ProbeList = std::vector<std::pair<std::string, int>>;
    std::vector<PluginInfo::ptr> probePlugins(const ProbeList& pluginList,
            ProbeCallback callback);
    // data
    std::string path_;
    std::unique_ptr<IModule> module_;
    std::vector<PluginInfo::ptr> plugins_;
    std::unordered_map<std::string, PluginInfo::ptr> pluginMap_;

};

} // vst
