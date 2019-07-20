#pragma once

#include "VSTPluginInterface.h"
#include "Utility.h"

#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <mutex>

namespace vst {

// thread-safe manager for VST plugins (factories and descriptions)

class VSTPluginManager {
 public:
    // factories
    void addFactory(const std::string& path, IVSTFactory::ptr factory);
    IVSTFactory::const_ptr findFactory(const std::string& path) const;
    // black-listed modules
    void addException(const std::string& path);
    bool isException(const std::string& path) const;
    // plugin descriptions
    void addPlugin(const std::string& key, VSTPluginDesc::const_ptr plugin);
    VSTPluginDesc::const_ptr findPlugin(const std::string& key) const;
    // remove factories and plugin descriptions
    void clear();
    // (de)serialize
    // throws an VSTError exception on failure!
    void read(const std::string& path, bool update = true);
    void write(const std::string& path);
 private:
    void doWrite(const std::string& path);
    std::unordered_map<std::string, IVSTFactory::ptr> factories_;
    std::unordered_map<std::string, VSTPluginDesc::const_ptr> plugins_;
    std::unordered_set<std::string> exceptions_;
    using Lock = std::lock_guard<std::mutex>;
    mutable std::mutex mutex_;
};

// implementation

void VSTPluginManager::addFactory(const std::string& path, IVSTFactory::ptr factory) {
    Lock lock(mutex_);
    factories_[path] = std::move(factory);
}

IVSTFactory::const_ptr VSTPluginManager::findFactory(const std::string& path) const {
    Lock lock(mutex_);
    auto factory = factories_.find(path);
    if (factory != factories_.end()){
        return factory->second;
    } else {
        return nullptr;
    }
}

void VSTPluginManager::addException(const std::string &path){
    Lock lock(mutex_);
    exceptions_.insert(path);
}

bool VSTPluginManager::isException(const std::string& path) const {
    Lock lock(mutex_);
    return exceptions_.count(path) != 0;
}

void VSTPluginManager::addPlugin(const std::string& key, VSTPluginDesc::const_ptr plugin) {
    Lock lock(mutex_);
    plugins_[key] = std::move(plugin);
}

VSTPluginDesc::const_ptr VSTPluginManager::findPlugin(const std::string& key) const {
    Lock lock(mutex_);
    auto desc = plugins_.find(key);
    if (desc != plugins_.end()){
        return desc->second;
    }
    return nullptr;
}

void VSTPluginManager::clear() {
    Lock lock(mutex_);
    factories_.clear();
    plugins_.clear();
    exceptions_.clear();
}

bool getLine(std::istream& stream, std::string& line);
int getCount(const std::string& line);

void VSTPluginManager::read(const std::string& path, bool update){
    Lock lock(mutex_);
    bool outdated = false;
    LOG_VERBOSE("reading cache file: " << path);
    File file(path);
    std::string line;
    while (getLine(file, line)){
        if (line == "[plugins]"){
            std::getline(file, line);
            int numPlugins = getCount(line);
            while (numPlugins--){
                // deserialize plugin
                auto desc = std::make_shared<VSTPluginDesc>();
                desc->deserialize(file);
                // collect keys
                std::vector<std::string> keys;
                while (getLine(file, line)){
                    if (line == "[keys]"){
                        std::getline(file, line);
                        int n = getCount(line);
                        while (n-- && std::getline(file, line)){
                            keys.push_back(std::move(line));
                        }
                        break;
                    } else {
                        throw VSTError("bad format");
                    }
                }
                // load the factory (if not loaded already) to verify that the plugin still exists
                IVSTFactory::ptr factory;
                if (!factories_.count(desc->path)){
                    try {
                        factory = IVSTFactory::load(desc->path);
                        factories_[desc->path] = factory;
                    } catch (const VSTError& e){
                        // this probably happens when the plugin has been (re)moved
                        LOG_ERROR("couldn't load '" << desc->name << "': " << e.what());
                        outdated = true; // we need to update the cache
                        continue; // skip plugin
                    }
                } else {
                    factory = factories_[desc->path];
                }
                factory->addPlugin(desc);
                desc->setFactory(factory);
                desc->probeResult = ProbeResult::success;
                for (auto& key : keys){
                    plugins_[key] = desc;
                }
            }
        } else if (line == "[ignore]"){
            std::getline(file, line);
            int numExceptions = getCount(line);
            while (numExceptions-- && std::getline(file, line)){
                exceptions_.insert(line);
            }
        } else {
            throw VSTError("bad data: " + line);
        }
    }
    if (update && outdated){
        // overwrite file
        file.close();
        try {
            doWrite(path);
        } catch (const VSTError& e){
            throw VSTError("couldn't update cache file");
        }
        LOG_VERBOSE("updated cache file");
    }
    LOG_DEBUG("done reading cache file");
}

void VSTPluginManager::write(const std::string &path){
    Lock lock(mutex_);
    doWrite(path);
}

void VSTPluginManager::doWrite(const std::string& path){
    LOG_DEBUG("writing cache file: " << path);
    File file(path, File::WRITE);
    if (!file.is_open()){
        throw VSTError("couldn't create file " + path);
    }
    // inverse mapping (plugin -> keys)
    std::unordered_map<VSTPluginDesc::const_ptr, std::vector<std::string>> pluginMap;
    for (auto& it : plugins_){
        if (it.second->valid()){
            pluginMap[it.second].push_back(it.first);
        }
    }
    // serialize plugins
    file << "[plugins]\n";
    file << "n=" << pluginMap.size() << "\n";
    for (auto& it : pluginMap){
        it.first->serialize(file);
        file << "[keys]\n";
        file << "n=" << it.second.size() << "\n";
        for (auto& key : it.second){
            file << key << "\n";
        }
    }
    // serialize exceptions
    file << "[ignore]\n";
    file << "n=" << exceptions_.size() << "\n";
    for (auto& e : exceptions_){
        file << e << "\n";
    }
}

} // vst
