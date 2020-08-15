#pragma once

#include "Interface.h"
#include "PluginCommand.h"
#include "Sync.h"

#include <thread>
#include <atomic>
#include <vector>

namespace vst {

class PluginServer;
class ShmInterface;
class ShmChannel;

/*///////////////// PluginHandle ///////////////*/

class PluginHandle : public IPluginListener {
 public:
    PluginHandle() = default;
    PluginHandle(PluginServer& server, IPlugin::ptr plugin, uint32_t id);

    void handleRequest(const ShmCommand& cmd, ShmChannel& channel);
    void handleUICommand(const ShmUICommand& cmd);
 private:
    void parameterAutomated(int index, float value) override;
    void latencyChanged(int nsamples) override;
    void midiEvent(const MidiEvent& event) override;
    void sysexEvent(const SysexEvent& event) override;

    void process(const ShmCommand& cmd, ShmChannel& channel);
    template<typename T>
    void doProcess(IPlugin::ProcessData<T>& data);

    void cacheParamState();
    void sendUpdate(ShmChannel& channel, bool bank);

    static void addReply(ShmChannel& channel, const void *cmd, size_t size = 0);

    PluginServer *server_ = nullptr;
    IPlugin::ptr plugin_;
    uint32_t id_ = 0;

    std::vector<double> input_;
    std::vector<double> auxInput_;
    std::vector<double> output_;
    std::vector<double> auxOuput_;
    std::vector<Command> commands_;

    struct ParamState {
        float value;
        std::string display;
    };
    std::unique_ptr<ParamState[]> paramState_;

    static void addParam(ShmChannel& channel, int index,
                         const ParamState& state, bool automated);
};

#define AddReply(cmd, field) addReply(&(cmd), (cmd).headerSize + sizeof((cmd).field))

/*////////////////// PluginServer ////////////////*/

class PluginServer {
 public:
    PluginServer(int pid, const std::string& shmPath);
    ~PluginServer();

    void run();

    void postUIThread(const ShmUICommand& cmd);
 private:
    void pollUIThread();
    void runThread(ShmChannel* channel);
    void quit();

    PluginHandle *findPlugin(uint32_t id);

    int pid_ = -1;
    std::unique_ptr<ShmInterface> shm_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_;

    std::unordered_map<uint32_t, PluginHandle> plugins_;
    SharedMutex pluginMutex_;
};

} // vst
