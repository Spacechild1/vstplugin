#pragma once

#include "Interface.h"
#include "PluginInfo.h"
#include "Sync.h"

#include <array>
#include <unordered_map>
#include <unordered_set>

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
    void write(const std::string& path) const;
    // read a single plugin description
    PluginInfo::const_ptr readPlugin(std::istream& stream);
 private:
    PluginInfo::const_ptr doReadPlugin(std::istream& stream, int versionMajor,
                                       int versionMinor, int versionPatch);
    void doWrite(const std::string& path) const;
    std::unordered_map<std::string, IFactory::ptr> factories_;
    enum {
        NATIVE = 0,
        BRIDGED = 1
    };
    std::array<std::unordered_map<std::string, PluginInfo::const_ptr>, 2> plugins_;
    std::unordered_set<std::string> exceptions_;
    mutable SharedMutex mutex_;
};

} // vst
