#pragma once

#include "Interface.h"
#include "ShmInterface.h"
#include "Sync.h"

#include <memory>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>

#ifdef _WIN32
#include "Windows.h"
#else
# include <unistd.h>
# include <stdio.h>
# include <sys/wait.h>
#endif

#ifndef BRIDGE_LOG
#define BRIDGE_LOG 0
#endif

namespace vst {

class PluginInfo;
enum class CpuArch;

/*/////////////////////// RTChannel / NRTChannel ////////////////////////*/

template<typename Mutex>
struct _Channel {
    _Channel(ShmChannel& channel)
        : channel_(&channel){}
    _Channel(ShmChannel& channel, Mutex& mutex)
        : channel_(&channel), lock_(std::unique_lock<Mutex>(mutex)){}

    bool addCommand(const void* cmd, size_t size = 0){
        return channel_->addMessage(static_cast<const char *>(cmd), size);
    }
#define AddCommand(cmd, field) addCommand(&(cmd), (cmd).headerSize + sizeof((cmd).field))

    void send(){
        channel_->post();
        channel_->waitReply();
    }

    template<typename T>
    bool getReply(const T *& reply){
        size_t dummy;
        return channel_->getMessage(*reinterpret_cast<const char **>(&reply), dummy);
    }
 private:
    ShmChannel *channel_;
    std::unique_lock<Mutex> lock_;
};

using RTChannel = _Channel<SpinLock>;
using NRTChannel = _Channel<SharedMutex>;

/*//////////////////////////// PluginBridge ///////////////////////////*/

class ShmNRTCommand;

class PluginBridge final
        : std::enable_shared_from_this<PluginBridge> {
 public:
    using ptr = std::shared_ptr<PluginBridge>;

    static PluginBridge::ptr getShared(CpuArch arch);
    static PluginBridge::ptr create(CpuArch arch);

    PluginBridge(CpuArch arch, bool shared);
    PluginBridge(const PluginBridge&) = delete;
    ~PluginBridge();

    bool shared() const {
        return shared_;
    }

    bool alive() const {
        return alive_.load(std::memory_order_acquire);
    }

    void checkStatus();

    void addUIClient(uint32_t id, std::shared_ptr<IPluginListener> client);

    void removeUIClient(uint32_t id);

    bool postUIThread(const ShmNRTCommand& cmd);

    RTChannel getRTChannel();

    NRTChannel getNRTChannel();
 private:
    static const int maxNumThreads = 8;
    static const size_t queueSize = 1024;
    static const size_t nrtRequestSize = 1024;
    static const size_t rtRequestSize = 65536;

    ShmInterface shm_;
    bool shared_;
    std::atomic_bool alive_{false};
#ifdef _WIN32
    PROCESS_INFORMATION pi_;
#else
    pid_t pid_;
#endif
    std::unique_ptr<SpinLock[]> locks_;
    std::unordered_map<uint32_t, std::weak_ptr<IPluginListener>> clients_;
    SharedMutex clientMutex_;
    SharedMutex nrtMutex_;
    // unnecessary, as all IWindow methods should be called form the same thread
    // SharedMutex uiMutex_;
    UIThread::Handle pollFunction_;

    void pollUIThread();
};

/*/////////////////////////// WatchDog //////////////////////////////*/

// there's a deadlock bug in the windows runtime library which would cause
// the process to hang if trying to join a thread in a static object destructor.
#ifdef _WIN32
# define WATCHDOG_JOIN 0
#else
# define WATCHDOG_JOIN 1
#endif

class WatchDog {
 public:
    static WatchDog& instance();

    ~WatchDog();

    void registerProcess(PluginBridge::ptr process);
 private:
    WatchDog();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool running_;
    std::vector<std::weak_ptr<PluginBridge>> processes_;
};

} // vst
