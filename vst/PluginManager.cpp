#include "PluginManager.h"
#include "Utility.h"

#include <fstream>
#include <cstdlib>
#include <sstream>
#include <algorithm>

namespace vst {

void PluginManager::addFactory(const std::string& path, IFactory::ptr factory) {
    WriteLock lock(mutex_);
    factories_[path] = std::move(factory);
}

IFactory::const_ptr PluginManager::findFactory(const std::string& path) const {
    ReadLock lock(mutex_);
    auto factory = factories_.find(path);
    if (factory != factories_.end()){
        return factory->second;
    } else {
        return nullptr;
    }
}

void PluginManager::addException(const std::string &path){
    WriteLock lock(mutex_);
    exceptions_.insert(path);
}

bool PluginManager::isException(const std::string& path) const {
    ReadLock lock(mutex_);
    return exceptions_.count(path) != 0;
}

void PluginManager::addPlugin(const std::string& key, PluginInfo::const_ptr plugin) {
    WriteLock lock(mutex_);
    plugins_[key] = std::move(plugin);
}

PluginInfo::const_ptr PluginManager::findPlugin(const std::string& key) const {
    ReadLock lock(mutex_);
    auto desc = plugins_.find(key);
    if (desc != plugins_.end()){
        return desc->second;
    }
    return nullptr;
}

void PluginManager::clear() {
    WriteLock lock(mutex_);
    factories_.clear();
    plugins_.clear();
    exceptions_.clear();
}

bool getLine(std::istream& stream, std::string& line);
int getCount(const std::string& line);

void PluginManager::read(const std::string& path, bool update){
    ReadLock lock(mutex_);
    int versionMajor = 0, versionMinor = 0, versionBugfix = 0;
    bool outdated = false;
    File file(path);
    std::string line;
    while (getLine(file, line)){
        if (line == "[version]"){
            std::getline(file, line);
            char *pos = (char *)line.c_str();
            if (*pos){
                versionMajor = std::strtol(pos, &pos, 10);
                if (*pos++ == '.'){
                    versionMinor = std::strtol(pos, &pos, 10);
                    if (*pos++ == '.'){
                        versionBugfix = std::strtol(pos, &pos, 10);
                    }
                }
            }
        } else if (line == "[plugins]"){
            std::getline(file, line);
            int numPlugins = getCount(line);
            while (numPlugins--){
                // deserialize plugin
                auto desc = std::make_shared<PluginInfo>(nullptr);
                desc->deserialize(file, versionMajor, versionMinor, versionBugfix);
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
                // scan presets
                desc->scanPresets();
                // load the factory (if not loaded already) to verify that the plugin still exists
                IFactory::ptr factory;
                if (!factories_.count(desc->path())){
                    try {
                        factory = IFactory::load(desc->path());
                        factories_[desc->path()] = factory;
                    } catch (const Error& e){
                        // this probably happens when the plugin has been (re)moved
                        LOG_ERROR("couldn't load '" << desc->name <<
                                  "' (" << desc->path() << "): " << e.what());
                        outdated = true; // we need to update the cache
                        continue; // skip plugin
                    }
                } else {
                    factory = factories_[desc->path()];
                }
                factory->addPlugin(desc);
                desc->setFactory(factory);
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
    LOG_DEBUG("read cache file " << path << " v" << versionMajor
              << "." << versionMinor << "." << versionBugfix);
}

void PluginManager::write(const std::string &path) const {
    WriteLock lock(mutex_);
    doWrite(path);
}

void PluginManager::doWrite(const std::string& path) const {
    File file(path, File::WRITE);
    if (!file.is_open()){
        throw Error("couldn't create file " + path);
    }
    // inverse mapping (plugin -> keys)
    std::unordered_map<PluginInfo::const_ptr, std::vector<std::string>> pluginMap;
    for (auto& it : plugins_){
        pluginMap[it.second].push_back(it.first);
    }
#if 0
    // actually, I'd like to sort alphabetically.
    // I've tried to do it  with a std::map, but VST2/VST3 plugins
    // of the same name either got lost or "merged"...
    // Something is wrong with this sort function.
    auto comp = [](const auto& lhs, const auto& rhs){
        std::string s1 = lhs.first->name;
        std::string s2 = rhs.first->name;
        for (auto& c : s1) { c = std::tolower(c); }
        for (auto& c : s2) { c = std::tolower(c); }
        if (s1 != s2){
            return s1 < s2;
        }
        if (lhs.first->type() != rhs.first->type()){
            return lhs.first->type() == IPlugin::VST3;
        }
        return false;
    };
#endif
    // write version number
    file << "[version]\n";
    file << VERSION_MAJOR << "." << VERSION_MINOR << "." << VERSION_BUGFIX << "\n";
    // serialize plugins
    file << "[plugins]\n";
    file << "n=" << pluginMap.size() << "\n";
    for (auto& it : pluginMap){
        // serialize plugin info
        it.first->serialize(file);
        // serialize keys
        file << "[keys]\n";
        auto& keys = it.second;
        file << "n=" << keys.size() << "\n";
        // sort by length, so that the short key comes first
        std::sort(keys.begin(), keys.end(), [](auto& a, auto& b){ return a.size() < b.size(); });
        for (auto& key : keys){
            file << key << "\n";
        }
    }
    // serialize exceptions
    file << "[ignore]\n";
    file << "n=" << exceptions_.size() << "\n";
    for (auto& e : exceptions_){
        file << e << "\n";
    }
    LOG_DEBUG("wrote cache file: " << path);
}

} // vst
