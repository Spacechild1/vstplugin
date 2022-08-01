#include "PluginDictionary.h"

#include "FileUtils.h"
#include "Log.h"
#if USE_WINE
# include "CpuArch.h"
#endif

#include <fstream>
#include <cstdlib>
#include <sstream>
#include <algorithm>

namespace vst {

void PluginDictionary::addFactory(const std::string& path, IFactory::ptr factory) {
    WriteLock lock(mutex_);
    factories_[path] = std::move(factory);
}

IFactory::const_ptr PluginDictionary::findFactory(const std::string& path) const {
    ReadLock lock(mutex_);
    auto factory = factories_.find(path);
    if (factory != factories_.end()){
        return factory->second;
    } else {
        return nullptr;
    }
}

void PluginDictionary::addException(const std::string &path){
    WriteLock lock(mutex_);
    exceptions_.insert(path);
}

bool PluginDictionary::isException(const std::string& path) const {
    ReadLock lock(mutex_);
    return exceptions_.count(path) != 0;
}

void PluginDictionary::addPlugin(const std::string& key, PluginDesc::const_ptr plugin) {
    WriteLock lock(mutex_);
    int index = plugin->bridged() ? BRIDGED : NATIVE;
#if USE_WINE
    if (index == BRIDGED){
        // prefer 64-bit Wine plugins
        auto it = plugins_[index].find(key);
        if (it != plugins_[index].end()){
            if (it->second->arch() == CpuArch::pe_amd64 &&
                    plugin->arch() == CpuArch::pe_i386) {
                LOG_DEBUG("ignore 32-bit Windows DLL");
                return;
            }
        }
    }
#endif
    // LOG_DEBUG("add plugin " << key << ((index == BRIDGED) ? " [bridged]" : ""));
    plugins_[index][key] = std::move(plugin);
}

PluginDesc::const_ptr PluginDictionary::findPlugin(const std::string& key) const {
    ReadLock lock(mutex_);
    // first try to find native plugin
    auto it = plugins_[NATIVE].find(key);
    if (it != plugins_[NATIVE].end()){
        return it->second;
    }
    // then try to find bridged plugin
    it = plugins_[BRIDGED].find(key);
    if (it != plugins_[BRIDGED].end()){
        return it->second;
    }
    return nullptr;
}

std::vector<PluginDesc::const_ptr> PluginDictionary::pluginList() const {
    ReadLock lock(mutex_);
    // inverse mapping (plugin -> keys)
    std::unordered_set<PluginDesc::const_ptr> pluginSet;
    for (auto& plugins : plugins_){
        for (auto& it : plugins){
            pluginSet.insert(it.second);
        }
    }
    std::vector<PluginDesc::const_ptr> plugins;
    plugins.reserve(pluginSet.size());
    for (auto& plugin : pluginSet){
        plugins.push_back(plugin);
    }
    return plugins;
}

void PluginDictionary::clear() {
    WriteLock lock(mutex_);
    factories_.clear();
    for (auto& plugins : plugins_){
        plugins.clear();
    }
    exceptions_.clear();
}

// PluginDesc.cpp
bool getLine(std::istream& stream, std::string& line);
int getCount(const std::string& line);

static double getPluginTimestamp(const std::string& path) {
    // LOG_DEBUG("getPluginTimestamp: " << path);
    if (isFile(path)) {
        return fileTimeLastModified(path);
    } else {
        // bundle: find newest timestamp of all contained binaries
        double timestamp = 0;
        vst::search(path + "/Contents", [&](auto& path){
            double t = fileTimeLastModified(path);
            if (t > timestamp) {
                timestamp = t;
            }
        }, false); // don't filter by extensions (because of macOS)!
        return timestamp;
    }
}

void PluginDictionary::read(const std::string& path, bool update){
    ReadLock lock(mutex_);
    int versionMajor = 0, versionMinor = 0, versionBugfix = 0;
    bool outdated = false;

    double timestamp = fileTimeLastModified(path);
    // LOG_DEBUG("cache file timestamp: " << timestamp);

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
                // there was a breaking change between 0.4 and 0.5
                // (introduction of audio input/output busses)
                if ((versionMajor < VERSION_MAJOR)
                        || (versionMajor == 0 && versionMinor < 5)){
                    throw Error(Error::PluginError,
                                "The plugin cache file is incompatible with this version. "
                                "Please perform a new search!");
                }
            }
        } else if (line == "[plugins]"){
            std::getline(file, line);
            int numPlugins = getCount(line);
            while (numPlugins--){
                // read a single plugin description
                auto plugin = doReadPlugin(file, timestamp, versionMajor,
                                           versionMinor, versionBugfix);
                // always collect keys, otherwise reading the cache file
                // would throw an error if a plugin had been removed
                std::vector<std::string> keys;
                std::string line;
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
                if (plugin){
                    LOG_DEBUG("read plugin " << plugin->name);
                    // store plugin at keys
                    for (auto& key : keys){
                        int index = plugin->bridged() ? BRIDGED : NATIVE;
                        plugins_[index][key] = plugin;
                    }
                } else {
                    // plugin has been changed or removed - update the cache
                    outdated = true;
                }
            }
        } else if (line == "[ignore]"){
            std::getline(file, line);
            int numExceptions = getCount(line);
            while (numExceptions-- && std::getline(file, line)){
                // check if plugin has been changed or removed
                if (pathExists(line)) {
                    try {
                        auto t = getPluginTimestamp(line);
                        if (t < timestamp) {
                            exceptions_.insert(line);
                        } else {
                            LOG_VERBOSE("black-listed plugin " << line << " has changed");
                            outdated = true;
                        }
                    } catch (const Error& e) {
                        LOG_ERROR("could not get timestamp for " << line << ": " << e.what());
                        outdated = true;
                    }
                } else {
                    LOG_VERBOSE("black-listed plugin " << line << " has been removed");
                    outdated = true;
                }
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
            throw Error("couldn't update cache file: " + std::string(e.what()));
        }
        LOG_VERBOSE("updated cache file");
    }
    LOG_DEBUG("cache file version: v" << versionMajor
              << "." << versionMinor << "." << versionBugfix);
}

PluginDesc::const_ptr PluginDictionary::readPlugin(std::istream& stream){
    WriteLock lock(mutex_);
    return doReadPlugin(stream, -1, VERSION_MAJOR,
                        VERSION_MINOR, VERSION_PATCH);
}

PluginDesc::const_ptr PluginDictionary::doReadPlugin(std::istream& stream, double timestamp,
                                                     int versionMajor, int versionMinor, int versionPatch){
    // deserialize plugin
    auto desc = std::make_shared<PluginDesc>(nullptr);
    try {
        desc->deserialize(stream, versionMajor, versionMinor, versionPatch);
    } catch (const Error& e){
        LOG_ERROR("couldn't deserialize plugin info for '" << desc->name << "': " << e.what());
        return nullptr;
    }

    // check if the plugin has been removed or changed since the last cache file update
    if (!pathExists(desc->path())) {
        LOG_WARNING("plugin " << desc->path() << " has been removed");
        return nullptr; // skip
    }
    try {
        if (timestamp >= 0 && getPluginTimestamp(desc->path()) > timestamp) {
            LOG_WARNING("plugin " << desc->path() << " has changed");
            return nullptr; // skip
        }
    } catch (const Error& e) {
        LOG_ERROR("could not get timestamp for " << desc->path()
                    << ": " << e.what());
        return nullptr;  // skip
    }
    // load the factory (if not already loaded)
    IFactory::ptr factory;
    if (!factories_.count(desc->path())){
        try {
            factory = IFactory::load(desc->path());
            factories_[desc->path()] = factory;
        } catch (const Error& e){
            LOG_WARNING("couldn't load '" << desc->name <<
                        "' (" << desc->path() << "): " << e.what());
            return nullptr; // skip
        }
    } else {
        factory = factories_[desc->path()];
        // check if plugin has already been added
        auto result = factory->findPlugin(desc->name);
        if (result){
            // return existing plugin descriptor
            return result;
        }
    }
    // associate plugin and factory
    desc->setFactory(factory);
    factory->addPlugin(desc);
    // scan presets
    desc->scanPresets();

    return desc;
}

void PluginDictionary::write(const std::string &path) const {
    WriteLock lock(mutex_);
    doWrite(path);
}

void PluginDictionary::doWrite(const std::string& path) const {
    File file(path, File::WRITE);
    if (!file.is_open()){
        throw Error("couldn't create file " + path);
    }
    // inverse mapping (plugin -> keys)
    std::unordered_map<PluginDesc::const_ptr, std::vector<std::string>> pluginMap;
    for (auto& plugins : plugins_){
        for (auto& it : plugins){
            pluginMap[it.second].push_back(it.first);
        }
    }
    // write version number
    file << "[version]\n";
    file << VERSION_MAJOR << "." << VERSION_MINOR << "." << VERSION_PATCH << "\n";
    // serialize exceptions
    // NOTE: do this before serializing the plugins because it is more robust;
    // otherwise we might get swallowed if a plugin desc is broken.
    file << "[ignore]\n";
    file << "n=" << exceptions_.size() << "\n";
    for (auto& e : exceptions_){
        file << e << "\n";
    }
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
    LOG_DEBUG("wrote cache file: " << path);
}

} // vst
