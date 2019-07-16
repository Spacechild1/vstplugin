#pragma once

#include "VSTPluginInterface.h"
#include "Utility.h"

#include <unordered_map>
#include <fstream>
#include <sstream>
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
    void read(const std::string& path, bool update = true);
    void write(const std::string& path);
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

bool isComment(const std::string& line);
int getCount(const std::string& line);

void VSTPluginManager::read(const std::string& path, bool update){
    std::lock_guard<std::mutex> lock(mutex_);
    bool outdated = false;
    LOG_DEBUG("reading cache file: " << path);
    File file(path);
    std::string line;
    while (std::getline(file, line)){
        // ignore empty lines + comments
        if (isComment(line) || line.empty()){
            continue;
        }
        int numPlugins = getCount(line);
        while (numPlugins--){
            auto desc = std::make_shared<VSTPluginDesc>();
            LOG_DEBUG("collect plugin");
            desc->deserialize(file);
            LOG_DEBUG("collect keys");
            // collect keys
            std::vector<std::string> keys;
            while (std::getline(file, line)){
                if (isComment(line)){
                    continue; // ignore comment
                }
                if (line == "[keys]"){
                    std::getline(file, line);
                    int n = getCount(line);
                    while (n-- && std::getline(file, line)){
                        keys.push_back(std::move(line));
                    }
                    break;
                } else {
                    throw VSTError("VSTPluginManager::read: bad format");
                }
            }

            // load the factory (if not loaded already) to verify that the plugin still exists
            IVSTFactory *factory = nullptr;
            if (!factories_.count(desc->path)){
                auto newFactory = IVSTFactory::load(desc->path);
                if (newFactory){
                    factory = newFactory.get();
                    factories_[desc->path] = std::move(newFactory);
                } else {
                    outdated = true;
                    continue; // plugin file has been (re)moved
                }
            } else {
                factory = factories_[desc->path].get();
            }
            factory->addPlugin(desc);
            desc->setFactory(factory);
            desc->probeResult = ProbeResult::success;
            for (auto& key : keys){
                plugins_[key] = desc;
            }
        }
    }
    if (update && outdated){
        // overwrite file
        file.close();
        try {
            write(path);
        } catch (const VSTError& e){
            throw VSTError("couldn't update cache file");
        }
        LOG_VERBOSE("updated cache file");
    }
}

void VSTPluginManager::write(const std::string& path){
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_DEBUG("writing cache file: " << path);
    File file(path, File::WRITE);
    if (!file.is_open()){
        throw VSTError("couldn't create file " + path);
    }
    // inverse mapping (plugin -> keys)
    std::unordered_map<VSTPluginDescPtr, std::vector<std::string>> pluginMap;
    for (auto& it : plugins_){
        pluginMap[it.second].push_back(it.first);
    }
    // serialize plugin + keys
    file << "n=" << pluginMap.size() << "\n";
    for (auto& it : pluginMap){
        it.first->serialize(file);
        file << "[keys]\n";
        file << "n=" << it.second.size() << "\n";
        for (auto& key : it.second){
            file << key << "\n";
        }
    }
}

} // vst
