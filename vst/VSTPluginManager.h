#pragma once

#include "VSTPluginInterface.h"

#include <unordered_map>
#include <mutex>

namespace vst {

// thread-safe manager for VST plugins (factories and descriptions)

class VSTPluginManager {
 public:
    // factories
    void addFactory(const std::string& path, IVSTFactoryPtr factory);
    IVSTFactory * findFactory(const std::string& path) const;
    // plugin descriptions
    void addPlugin(const std::string& key, VSTPluginDescPtr plugin);
    VSTPluginDescPtr findPlugin(const std::string& key) const;
    void clearPlugins();
 private:
    std::unordered_map<std::string, IVSTFactoryPtr> factories_;
    std::unordered_map<std::string, VSTPluginDescPtr> plugins_;
    mutable std::mutex mutex_;
};

void VSTPluginManager::addFactory(const std::string& path, IVSTFactoryPtr factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    factories_[path] = std::move(factory);
}
IVSTFactory * VSTPluginManager::findFactory(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto factory = factories_.find(path);
    if (factory != factories_.end()){
        return factory->second.get();
    } else {
        return nullptr;
    }
}
void VSTPluginManager::addPlugin(const std::string& key, VSTPluginDescPtr plugin) {
    std::lock_guard<std::mutex> lock(mutex_);
    plugins_[key] = std::move(plugin);
}
VSTPluginDescPtr VSTPluginManager::findPlugin(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto desc = plugins_.find(key);
    if (desc != plugins_.end()){
        return desc->second;
    }
    return nullptr;
}
void VSTPluginManager::clearPlugins() {
    std::lock_guard<std::mutex> lock(mutex_);
    plugins_.clear();
}

} // vst
