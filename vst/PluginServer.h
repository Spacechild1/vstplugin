#pragma once

#include "Interface.h"
#include "PluginCommand.h"
#include "Sync.h"
#include "Bus.h"
#include "Lockfree.h"

#include <thread>
#include <atomic>
#include <vector>

// On Wine, checking the parent process works
// better with the native host system API
#if VST_HOST_SYSTEM == VST_WINDOWS
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#else
# include <sys/types.h>
# include <unistd.h>
#endif

#ifndef DEBUG_SERVER_PROCESS
#define DEBUG_SERVER_PROCESS 0
#endif

namespace vst {

class PluginServer;
class ShmInterface;
class ShmChannel;

/*///////////////// PluginHandle ///////////////*/

class PluginHandle :
    public IPluginListener,
    public std::enable_shared_from_this<PluginHandle>
{
public:
    PluginHandle(PluginServer& server, IPlugin::ptr plugin,
                 uint32_t id, ShmChannel& channel);
    ~PluginHandle();

    void init();

    void handleRequest(const ShmCommand& cmd, ShmChannel& channel);
    void handleUICommand(const ShmUICommand& cmd);

    void parameterAutomated(int index, float value) override;
    void latencyChanged(int nsamples) override;
    void updateDisplay() override;
    void pluginCrashed() override {} // never called inside the bridge
    void midiEvent(const MidiEvent& event) override;
    void sysexEvent(const SysexEvent& event) override;
private:
    friend class PluginHandleListener;

    void updateBuffer();

    void process(const ShmCommand& cmd, ShmChannel& channel);

    template<typename T>
    void doProcess(const ShmCommand& cmd, ShmChannel& channel);

    void dispatchCommands(ShmChannel& channel);

    void sendEvents(ShmChannel& channel);

    void sendParameterUpdate(ShmChannel& channel);
    void sendProgramUpdate(ShmChannel& channel, bool bank);

    static bool addReply(ShmChannel& channel, const void *cmd, size_t size = 0);

    PluginServer *server_ = nullptr;
    IPlugin::ptr plugin_;
    uint32_t id_ = 0;

    int maxBlockSize_ = 64;
    ProcessPrecision precision_{ProcessPrecision::Single};
    std::unique_ptr<Bus[]> inputs_;
    int numInputs_ = 0;
    std::unique_ptr<Bus[]> outputs_;
    int numOutputs_ = 0;
    std::vector<char> buffer_;
    std::vector<Command> events_;

    // parameter automation from GUI
    // -> ask RT thread to send parameter state
    struct Param {
        int32_t index;
        float value;
    };
    UnboundedMPSCQueue<Param> paramAutomated_;
    std::atomic<bool> updateDisplay_{false};

    static const int paramAutomationRateLimit = 64;

    // cached parameter state
    std::unique_ptr<float[]> paramState_;

    void sendParam(ShmChannel& channel, int index,
                   float value, bool automated);
};

#define AddReply(cmd, field) addReply(&(cmd), (cmd).headerSize + sizeof((cmd).field))

/*////////////////// PluginServer ////////////////*/

class PluginServer {
 public:
    PluginServer(int pid, const std::string& shmPath);
    ~PluginServer();

    void run();

    bool postUIThread(const ShmUICommand& cmd);
 private:
    void pollUIThread();
    void checkIfParentAlive();
    void runThread(ShmChannel* channel);
    void handleCommand(ShmChannel& channel,
                       const ShmCommand &cmd);
    void quit();

    void createPlugin(uint32_t id, const char *data, size_t size,
                      ShmChannel& channel);

    void destroyPlugin(uint32_t id);

    PluginHandle *findPlugin(uint32_t id);

    // NOTE: UI thread order is the opposite of PluginBridge!
    struct Channel  {
        enum {
            UIReceive = 0,
            UISend,
            NRT
        };
    };

#if VST_HOST_SYSTEM == VST_WINDOWS
    HANDLE parent_ = NULL;
#else
    int parent_ = -1;
#endif
    std::unique_ptr<ShmInterface> shm_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_;
    UIThread::Handle pollFunction_;

    std::unordered_map<uint32_t, std::shared_ptr<PluginHandle>> plugins_;
    SharedMutex pluginMutex_;
};

} // vst
