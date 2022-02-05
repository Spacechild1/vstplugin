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
# include <windows.h>
#else
# include <unistd.h>
# include <stdio.h>
# include <poll.h>
# include <sys/wait.h>
# include <fcntl.h>
# include <string.h>
#endif

// redirect stdout and stderr from child process to parent.
// use this if you want to see debug output from the actual VST plugins.
// NOTE: this doesn't affect log functions like LOG_ERROR because
// they go to a seperate pipe.
#ifndef BRIDGE_LOG
#define BRIDGE_LOG 0
#endif

namespace vst {

struct PluginDesc;
enum class CpuArch;

/*/////////////////////// RTChannel / NRTChannel ////////////////////////*/

// NOTE: if you want to unlock a _Channel prematurely, just let it go out of scope.
// Don't add an unlock() method (the internal lock might be already unlocked)!
template<typename Mutex>
struct _Channel {
    _Channel(ShmChannel& channel)
        : channel_(&channel)
        { channel.clear(); }
    _Channel(ShmChannel& channel, std::unique_lock<Mutex> lock)
        : channel_(&channel), lock_(std::move(lock))
        { channel.clear(); }
    _Channel(_Channel&& other)
        : channel_(other.channel_), lock_(std::move(other.lock_)) {}

    int32_t capacity() const { return channel_->capacity(); }

    bool addCommand(const void* cmd, size_t size){
        return channel_->addMessage(cmd, size);
    }

    void send(){
        channel_->post();
        channel_->waitReply();
    }

    template<typename T>
    bool getReply(const T *& reply, size_t& size){
        return channel_->getMessage(reinterpret_cast<const void *&>(reply), size);
    }

    template<typename T>
    bool getReply(const T *& reply){
        size_t dummy;
        return getReply(reply, dummy);
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

    void readLog();

    void checkStatus(bool wait);

    void addUIClient(uint32_t id, std::shared_ptr<IPluginListener> client);

    void removeUIClient(uint32_t id);

    void postUIThread(const ShmUICommand& cmd);

    RTChannel getRTChannel();

    NRTChannel getNRTChannel();
 private:
    static const size_t queueSize = 1024;
    static const size_t nrtRequestSize = 65536;
    static const size_t rtRequestSize = 65536;

    // NOTE: UI thread order is the opposite of PluginServer!
    struct Channel {
        enum {
            UISend = 0,
            UIReceive,
            NRT
        };
    };

    ShmInterface shm_;
    bool shared_;
    std::atomic_bool alive_{false};
#ifdef _WIN32
    HANDLE hLogRead_ = NULL;
    HANDLE hLogWrite_ = NULL;
    PROCESS_INFORMATION pi_;
#else
    pid_t pid_;
    int logRead_ = -1;
#endif
    uint32_t threadMask_ = 0;
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

class WatchDog {
 public:
    static WatchDog& instance();

    ~WatchDog();

    void registerProcess(PluginBridge::ptr process);
 private:
    WatchDog();

    void run();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool running_;
    std::vector<std::weak_ptr<PluginBridge>> processes_;
};

} // vst
