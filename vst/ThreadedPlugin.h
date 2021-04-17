#pragma once

#include "Interface.h"
#include "Sync.h"
#include "DeferredPlugin.h"
#include "LockfreeFifo.h"
#include "Bus.h"

#include <thread>

namespace vst {

class ThreadedPlugin;

class DSPThreadPool {
 public:
    static DSPThreadPool& instance(){
        static DSPThreadPool inst;
        return inst;
    }

    DSPThreadPool();
    ~DSPThreadPool();

    using Callback = void (*)(ThreadedPlugin *, int);
    bool push(Callback cb, ThreadedPlugin *plugin, int numSamples);
 private:
    struct Task {
        Callback cb;
        ThreadedPlugin *plugin;
        int numSamples;
    };
    LockfreeFifo<Task, 1024> queue_;
    std::vector<std::thread> threads_;
    Event event_;
    std::atomic<bool> running_;
    PaddedSpinLock pushLock_;
    PaddedSpinLock popLock_;
};

/*//////////////////// ThreadedPlugin ////////////////*/

class ThreadedPluginListener;

class ThreadedPlugin final : public DeferredPlugin
{
 public:
    friend class ThreadedPluginListener;

    ThreadedPlugin(IPlugin::ptr plugin);
    ~ThreadedPlugin();

    const PluginInfo& info() const override {
        return plugin_->info();
    }

    void setupProcessing(double sampleRate, int maxBlockSize, ProcessPrecision precision) override;
    void process(ProcessData& data) override;
    void suspend() override;
    void resume() override;
    void setNumSpeakers(int *input, int numInputs, int *output, int numOutputs) override;
    int getLatencySamples() override {
        return plugin_->getLatencySamples();
    }

    void setListener(IPluginListener::ptr listener) override;

    double getTransportPosition() const override {
        return plugin_->getTransportPosition();
    }

    float getParameter(int index) const override;
    std::string getParameterString(int index) const override;

    void setProgramName(const std::string& name) override;
    int getProgram() const override;
    std::string getProgramName() const override;
    std::string getProgramNameIndexed(int index) const override;

    void readProgramFile(const std::string& path) override;
    void readProgramData(const char *data, size_t size) override;
    void writeProgramFile(const std::string& path) override;
    void writeProgramData(std::string& buffer) override;
    void readBankFile(const std::string& path) override;
    void readBankData(const char *data, size_t size) override;
    void writeBankFile(const std::string& path) override;
    void writeBankData(std::string& buffer) override;

    void openEditor(void *window) override {
        plugin_->openEditor(window);
    }
    void closeEditor() override {
        plugin_->closeEditor();
    }
    bool getEditorRect(Rect& rect) const override {
        return plugin_->getEditorRect(rect);
    }
    void updateEditor() override {
        plugin_->updateEditor();
    }
    void checkEditorSize(int &width, int &height) const override {
        plugin_->checkEditorSize(width, height);
    }
    void resizeEditor(int width, int height) override {
        plugin_->resizeEditor(width, height);
    }
    bool canResize() const override {
        return plugin_->canResize();
    }
    void setWindow(IWindow::ptr window) override {
        plugin_->setWindow(std::move(window));
    }
    IWindow *getWindow() const override {
        return plugin_->getWindow();
    }

    // VST2 only
    int canDo(const char *what) const override {
        return plugin_->canDo(what);
    }
    intptr_t vendorSpecific(int index, intptr_t value, void *p, float opt) override;

    // VST3 only
    void beginMessage() override {
        plugin_->beginMessage();
    }
    void addInt(const char* id, int64_t value) override {
        plugin_->addInt(id, value);
    }
    void addFloat(const char* id, double value) override {
        plugin_->addFloat(id, value);
    }
    void addString(const char* id, const char *value) override {
        plugin_->addString(id, value);
    }
    void addString(const char* id, const std::string& value) override {
        plugin_->addString(id, value);
    }
    void addBinary(const char* id, const char *data, size_t size) override {
        plugin_->addBinary(id, data, size);
    }
    void endMessage() override {
        plugin_->endMessage();
    }
 private:
    void updateBuffer();
    template<typename T>
    void doProcess(ProcessData& data);
    void dispatchCommands();
    void sendEvents();
    template<typename T>
    void threadFunction(int numSamples);
    // data
    DSPThreadPool *threadPool_;
    IPlugin::ptr plugin_;
    std::weak_ptr<IPluginListener> listener_;
    std::shared_ptr<ThreadedPluginListener> proxyListener_;
    mutable Mutex mutex_;
    SyncCondition event_;
    std::thread::id rtThread_;
    // commands/events
    void pushCommand(const Command& command) override {
        commands_[current_].push_back(command);
    }
    void pushEvent(const Command& event){
        events_[!current_].push_back(event);
    }
    std::vector<Command> commands_[2];
    std::vector<Command> events_[2];
    int current_ = 0;
    // buffer
    int blockSize_ = 0;
    ProcessPrecision precision_ = ProcessPrecision::Single;
    std::unique_ptr<Bus[]> inputs_;
    int numInputs_ = 0;
    std::unique_ptr<Bus[]> outputs_;
    int numOutputs_ = 0;
    std::vector<char> buffer_;
};

/*/////////////////// ThreadedPluginListener ////////////////////*/

class ThreadedPluginListener : public IPluginListener {
 public:
    ThreadedPluginListener(ThreadedPlugin& owner)
        : owner_(&owner) {}
    void parameterAutomated(int index, float value) override;
    void latencyChanged(int nsamples) override;
    void pluginCrashed() override;
    void midiEvent(const MidiEvent& event) override;
    void sysexEvent(const SysexEvent& event) override;
 private:
    ThreadedPlugin *owner_;
};

} // vst
