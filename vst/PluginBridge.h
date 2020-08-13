#pragma once

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

struct RTChannel {
    RTChannel(ShmChannel& channel);
    RTChannel(ShmChannel& channel, SpinLock& lock);
    ~RTChannel();
    bool addCommand(const char *data, size_t size);
    void send();
    bool getReply(const char *&data, size_t& size);
 private:
    ShmChannel *channel_;
    SpinLock *lock_;
};

class IPluginListener;

class PluginBridge final
        : std::enable_shared_from_this<PluginBridge> {
 public:
    using ptr = std::shared_ptr<PluginBridge>;

    static PluginBridge::ptr getShared(CpuArch arch);
    static PluginBridge::ptr create(CpuArch arch, const PluginInfo& desc);

    PluginBridge(CpuArch arch, const PluginInfo* desc);
    PluginBridge(const PluginBridge&) = delete;
    ~PluginBridge();

    bool alive() const {
        return alive_.load(std::memory_order_acquire);
    }

    void checkStatus();

    using ID = uint32_t;

    void addUIClient(ID id, std::shared_ptr<IPluginListener> client);

    void removeUIClient(ID id);

    bool postUIThread(const char *cmd, size_t size);

    bool pollUIThread(char *buffer, size_t& size);

    RTChannel getChannel();
 private:
    static const int maxNumThreads = 8;
    static const size_t queueSize = 1024;
    static const size_t requestSize = 65536;

    ShmInterface shm_;
    std::atomic_bool alive_{false};
#ifdef _WIN32
    PROCESS_INFORMATION pi_;
#else
    pid_t pid_;
#endif
    std::unique_ptr<SpinLock[]> locks_;
    std::unordered_map<ID, std::weak_ptr<IPluginListener>> clients_;
    SharedMutex clientMutex_;
    SharedMutex uiMutex_; // probably unnecessary
};

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
