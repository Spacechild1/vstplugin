#pragma once

#include "Interface.h"
#include "PluginDesc.h"
#include "Sync.h"

#include <array>
#include <unordered_map>
#include <unordered_set>

namespace vst {

// thread-safe dictionary for VST plugins (factories and descriptions)

class PluginDictionary {
 public:
    // factories
    void addFactory(const std::string& path, IFactory::ptr factory);
    IFactory::const_ptr findFactory(const std::string& path) const;
    // black-listed modules
    void addException(const std::string& path);
    bool isException(const std::string& path) const;
    // plugin descriptions
    void addPlugin(const std::string& key, PluginDesc::const_ptr plugin);
    PluginDesc::const_ptr findPlugin(const std::string& key) const;
    std::vector<PluginDesc::const_ptr> pluginList() const;
    // remove factories and plugin descriptions
    void clear();
    // (de)serialize
    // throws an Error exception on failure!
    void read(const std::string& path, bool update = true);
    void write(const std::string& path) const;
    // read a single plugin description
    PluginDesc::const_ptr readPlugin(std::istream& stream);
 private:
    PluginDesc::const_ptr doReadPlugin(std::istream& stream, double timestamp,
                                       int versionMajor, int versionMinor, int versionPatch);
    void doWrite(const std::string& path) const;
    std::unordered_map<std::string, IFactory::ptr> factories_;
    enum {
        NATIVE = 0,
        BRIDGED = 1
    };
    std::array<std::unordered_map<std::string, PluginDesc::const_ptr>, 2> plugins_;
    std::unordered_set<std::string> exceptions_;
    mutable SharedMutex mutex_;
};

} // vst
