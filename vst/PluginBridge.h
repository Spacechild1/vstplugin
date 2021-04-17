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
# ifndef NOMINMAX
#  define NOMINMAX
# endif
#include "windows.h"
#else
# include <unistd.h>
# include <stdio.h>
# include <sys/wait.h>
#endif

#ifndef BRIDGE_LOG
#define BRIDGE_LOG 0
#endif

namespace vst {

struct PluginInfo;
enum class CpuArch;

/*/////////////////////// RTChannel / NRTChannel ////////////////////////*/

template<typename Mutex>
struct _Channel {
    _Channel(ShmChannel& channel)
        : channel_(&channel)
        { channel.clear(); }
    _Channel(ShmChannel& channel, std::unique_lock<Mutex> lock)
        : channel_(&channel),
          lock_(std::move(lock))
        { channel.clear(); }

    bool addCommand(const void* cmd, size_t size){
        return channel_->addMessage(static_cast<const char *>(cmd), size);
    }

    void send(){
        channel_->post();
        channel_->waitReply();
    }

    template<typename T>
    bool getReply(const T *& reply){
        size_t dummy;
        return channel_->getMessage(*reinterpret_cast<const char **>(&reply), dummy);
    }

    template<typename T>
    bool getReply(const T *& reply, size_t& size){
        return channel_->getMessage(*reinterpret_cast<const char **>(&reply), size);
    }

    void checkError();
 private:
    ShmChannel *channel_;
    std::unique_lock<Mutex> lock_;
};

#define AddCommand(cmd, field) addCommand(&(cmd), (cmd).headerSize + sizeof((cmd).field))

using RTChannel = _Channel<PaddedSpinLock>;
using NRTChannel = _Channel<Mutex>;

/*//////////////////////////// PluginBridge ///////////////////////////*/

struct ShmUICommand;

class PluginBridge final
        : public std::enable_shared_from_this<PluginBridge> {
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

    void checkStatus(bool wait = false);

    void addUIClient(uint32_t id, std::shared_ptr<IPluginListener> client);

    void removeUIClient(uint32_t id);

    void postUIThread(const ShmUICommand& cmd);

    RTChannel getRTChannel();

    NRTChannel getNRTChannel();
 private:
    static const int maxNumThreads = 8;
    static const size_t queueSize = 1024;
    static const size_t nrtRequestSize = 65536;
    static const size_t rtRequestSize = 65536;

    ShmInterface shm_;
    bool shared_;
    std::atomic_bool alive_{false};
#ifdef _WIN32
    PROCESS_INFORMATION pi_;
#else
    pid_t pid_;
#endif
    std::unique_ptr<PaddedSpinLock[]> locks_;
    std::unordered_map<uint32_t, std::weak_ptr<IPluginListener>> clients_;
    Mutex clientMutex_;
    Mutex nrtMutex_;
    // unnecessary, as all IWindow methods should be called form the same thread
    // Mutex uiMutex_;
    UIThread::Handle pollFunction_;

    void pollUIThread();

    IPluginListener::ptr findClient(uint32_t id);

    void getStatus(bool wait);
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
