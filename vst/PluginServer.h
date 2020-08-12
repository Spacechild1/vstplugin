#pragma once

#include <string>
#include <memory>

namespace vst {

class ShmInterface;

class PluginServer {
 public:
    PluginServer(int pid, const std::string& shmPath);
    PluginServer(int pid, const std::string& shmPath,
                 const std::string& pluginPath,
                 const std::string& pluginName);
    ~PluginServer();
    void run();
 private:
    int pid_ = -1;
    std::unique_ptr<ShmInterface> shm_;
    std::string pluginPath_;
    std::string pluginName_;
};

} // vst
