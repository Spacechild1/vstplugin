#include "PluginServer.h"
#include "ShmInterface.h"

namespace vst {

PluginServer::PluginServer(int pid, const std::string& shmPath)
    : pid_(pid), shm_(std::make_unique<ShmInterface>())
{
    shm_->connect(shmPath);
}

PluginServer::PluginServer(int pid, const std::string& shmPath,
                           const std::string& pluginPath,
                           const std::string& pluginName)
    : PluginServer(pid, shmPath)
{
    pluginPath_ = pluginPath;
    pluginName_ = pluginName;
}

PluginServer::~PluginServer(){}

void PluginServer::run(){

}

} // vst
