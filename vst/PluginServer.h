#pragma once

#include <string>
#include <memory>

namespace vst {

class ShmInterface;

class PluginServer {
 public:
    PluginServer(int pid, const std::string& shmPath);
    ~PluginServer();
    void run();
 private:
    int pid_ = -1;
    std::unique_ptr<ShmInterface> shm_;
};

} // vst
