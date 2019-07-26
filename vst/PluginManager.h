#pragma once

#include "Interface.h"
#include "Utility.h"

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <mutex>

namespace vst {

// thread-safe manager for VST plugins (factories and descriptions)

class PluginManager {
 public:
    // factories
    void addFactory(const std::string& path, IFactory::ptr factory);
    IFactory::const_ptr findFactory(const std::string& path) const;
    // black-listed modules
    void addException(const std::string& path);
    bool isException(const std::string& path) const;
    // plugin descriptions
    void addPlugin(const std::string& key, PluginInfo::const_ptr plugin);
    PluginInfo::const_ptr findPlugin(const std::string& key) const;
    // remove factories and plugin descriptions
    void clear();
    // (de)serialize
    // throws an Error exception on failure!
    void read(const std::string& path, bool update = true);
    void write(const std::string& path);
 private:
    void doWrite(const std::string& path);
    std::unordered_map<std::string, IFactory::ptr> factories_;
    std::unordered_map<std::string, PluginInfo::const_ptr> plugins_;
    std::unordered_set<std::string> exceptions_;
    using Lock = std::lock_guard<std::mutex>;
    mutable std::mutex mutex_;
};

// implementation

void PluginManager::addFactory(const std::string& path, IFactory::ptr factory) {
    Lock lock(mutex_);
    factories_[path] = std::move(factory);
}

IFactory::const_ptr PluginManager::findFactory(const std::string& path) const {
    Lock lock(mutex_);
    auto factory = factories_.find(path);
    if (factory != factories_.end()){
        return factory->second;
    } else {
        return nullptr;
    }
}

void PluginManager::addException(const std::string &path){
    Lock lock(mutex_);
    exceptions_.insert(path);
}

bool PluginManager::isException(const std::string& path) const {
    Lock lock(mutex_);
    return exceptions_.count(path) != 0;
}

void PluginManager::addPlugin(const std::string& key, PluginInfo::const_ptr plugin) {
    Lock lock(mutex_);
    plugins_[key] = std::move(plugin);
}

PluginInfo::const_ptr PluginManager::findPlugin(const std::string& key) const {
    Lock lock(mutex_);
    auto desc = plugins_.find(key);
    if (desc != plugins_.end()){
        return desc->second;
    }
    return nullptr;
}

void PluginManager::clear() {
    Lock lock(mutex_);
    factories_.clear();
    plugins_.clear();
    exceptions_.clear();
}

bool getLine(std::istream& stream, std::string& line);
int getCount(const std::string& line);

void PluginManager::read(const std::string& path, bool update){
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
                auto desc = std::make_shared<PluginInfo>();
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
                        throw Error("bad format");
                    }
                }
                // load the factory (if not loaded already) to verify that the plugin still exists
                IFactory::ptr factory;
                if (!factories_.count(desc->path)){
                    try {
                        factory = IFactory::load(desc->path);
                        factories_[desc->path] = factory;
                    } catch (const Error& e){
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
            throw Error("bad data: " + line);
        }
    }
    if (update && outdated){
        // overwrite file
        file.close();
        try {
            doWrite(path);
        } catch (const Error& e){
            throw Error("couldn't update cache file");
        }
        LOG_VERBOSE("updated cache file");
    }
    LOG_DEBUG("done reading cache file");
}

void PluginManager::write(const std::string &path){
    Lock lock(mutex_);
    doWrite(path);
}

void PluginManager::doWrite(const std::string& path){
    LOG_DEBUG("writing cache file: " << path);
    File file(path, File::WRITE);
    if (!file.is_open()){
        throw Error("couldn't create file " + path);
    }
    // inverse mapping (plugin -> keys), alphabetically sorted
    auto comp = [](const auto& lhs, const auto& rhs){
        std::string s1 = lhs->name;
        std::string s2 = rhs->name;
        for (auto& c : s1) { c = std::tolower(c); }
        for (auto& c : s2) { c = std::tolower(c); }
        return s1 < s2;
    };
    std::map<PluginInfo::const_ptr, std::vector<std::string>, decltype(comp)> pluginMap(comp);
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
