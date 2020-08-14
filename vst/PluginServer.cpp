#include "PluginServer.h"
#include "ShmInterface.h"

namespace vst {

PluginServer::PluginServer(int pid, const std::string& shmPath)
    : pid_(pid), shm_(std::make_unique<ShmInterface>())
{
    shm_->connect(shmPath);
}

PluginServer::~PluginServer(){}

void PluginServer::run(){

}

} // vst
