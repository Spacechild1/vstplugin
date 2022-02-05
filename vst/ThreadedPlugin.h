#pragma once

#include "Interface.h"
#include "Sync.h"
#include "DeferredPlugin.h"
#include "Lockfree.h"
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

    bool processTask();
 private:
    struct Task {
        Callback cb;
        ThreadedPlugin *plugin;
        int numSamples;
    };
    std::vector<std::thread> threads_;
    // NOTE: Semaphore is the right tool to notify one or more threads in a thread pool.
    // With Event there are certain edge cases where it would fail to notify the correct
    // number of threads. For example, if several worker threads are about to call wait()
    // more or less simultaneously and you call set() several times *before* one of them
    // actually manages to do so, only a single worker thread will continue and the others
    // will wait.
    // The only disadvantage of Semaphore is that all those post() calls will make the worker
    // threads spin a few times, but I think this negligible. Also, the post() call is a bit faster.
    LightSemaphore semaphore_;
    std::atomic<bool> running_;
    LockfreeFifo<Task, 1024> queue_;
    PaddedSpinLock pushLock_;
    PaddedSpinLock popLock_;

    void run();
};

/*//////////////////// ThreadedPlugin ////////////////*/

class ThreadedPluginListener;

class ThreadedPlugin final : public DeferredPlugin
{
 public:
    friend class ThreadedPluginListener;

    ThreadedPlugin(IPlugin::ptr plugin);
    ~ThreadedPlugin();

    const PluginDesc& info() const override {
        return plugin_->info();
    }

    void setupProcessing(double sampleRate, int maxBlockSize,
                         ProcessPrecision precision, ProcessMode mode) override;
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
    Event event_;
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
    ProcessMode mode_ = ProcessMode::Realtime;
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
    void updateDisplay() override;
    void pluginCrashed() override;
    void midiEvent(const MidiEvent& event) override;
    void sysexEvent(const SysexEvent& event) override;
 private:
    ThreadedPlugin *owner_;
};

} // vst
