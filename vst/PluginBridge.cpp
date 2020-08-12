#include "PluginBridge.h"

#include "Interface.h"
#include "Utility.h"
#include "Sync.h"

#include <unordered_map>

namespace vst {

SharedMutex gPluginBridgeMutex;

// use std::weak_ptr, so the bridge is automatically closed if it is not used
std::unordered_map<CpuArch, std::weak_ptr<PluginBridge>> gPluginBridgeMap;

PluginBridge::ptr PluginBridge::getShared(CpuArch arch){
    PluginBridge::ptr bridge;

    LockGuard lock(gPluginBridgeMutex);

    auto it = gPluginBridgeMap.find(arch);
    if (it != gPluginBridgeMap.end()){
        bridge = it->second.lock();
    }

    if (!bridge){
        // create shared bridge
        bridge = std::make_shared<PluginBridge>(arch, nullptr);
        gPluginBridgeMap.emplace(arch, bridge);
    }

    return bridge;
}

PluginBridge::ptr PluginBridge::create(CpuArch arch, const PluginInfo &desc){
    return std::make_shared<PluginBridge>(arch, &desc);
}


PluginBridge::PluginBridge(CpuArch arch, const PluginInfo *desc){
    // create process and setup shared memory interface
}

PluginBridge::~PluginBridge(){
    // send quit message and wait for process to finish
}

} // vst
