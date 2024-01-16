#pragma once

#include "Interface.h"
#include "PluginDesc.h"
#include "DeferredPlugin.h"
#include "MiscUtils.h"
#include "PluginBridge.h"

#include <array>

#ifndef DEBUG_CLIENT_PROCESS
#define DEBUG_CLIENT_PROCESS 0
#endif

namespace vst {

class alignas(CACHELINE_SIZE) PluginClient final : public DeferredPlugin {
public:
    PluginClient(IFactory::const_ptr f, PluginDesc::const_ptr desc,
                 bool sandbox, bool editor);

    virtual ~PluginClient();

    const PluginDesc& info() const override {
        return *info_;
    }

    PluginBridge& bridge() {
        return *bridge_;
    }

    bool check();

    uint32_t id() const { return id_; }

    void setupProcessing(double sampleRate, int maxBlockSize,
                         ProcessPrecision precision, ProcessMode mode) override;
    void process(ProcessData& data) override;
    void suspend() override;
    void resume() override;
    void setNumSpeakers(int *input, int numInputs, int *output, int numOutputs) override;
    int getLatencySamples() override;

    void setListener(IPluginListener* listener) override;

    double getTransportPosition() const override;

    void setParameter(int index, float value, int sampleOffset) override;
    bool setParameter(int index, const std::string& str, int sampleOffset) override;
    float getParameter(int index) const override;
    std::string getParameterString(int index) const override;

    void setProgram(int index) override;
    int getProgram() const override;
    void setProgramName(const std::string& name) override;
    std::string getProgramName() const override;
    std::string getProgramNameIndexed(int index) const override;

    // the following methods throw an Error exception on failure!
    void readProgramFile(const std::string& path) override;
    void readProgramData(const char *data, size_t size) override;
    void readProgramData(const std::string& buffer) {
        readProgramData(buffer.data(), buffer.size());
    }
    void readBankFile(const std::string& path) override;
    void readBankData(const char *data, size_t size) override;
    void readBankData(const std::string& buffer) {
        readBankData(buffer.data(), buffer.size());
    }
    void sendFile(Command::Type type, const std::string& path);
    void sendData(Command::Type type, const char *data, size_t size);

    void writeProgramFile(const std::string& path) override;
    void writeProgramData(std::string& buffer) override;
    void writeBankFile(const std::string& path) override;
    void writeBankData(std::string& buffer) override;

    void receiveData(Command::Type type, std::string& buffer);

    void openEditor(void *window) override;
    void closeEditor() override;
    bool getEditorRect(Rect& rect) const override;
    void updateEditor() override;
    void checkEditorSize(int& width, int& height) const override;
    void resizeEditor(int width, int height) override;
    bool canResize() const override;

    IWindow* getWindow() const override {
        return window_.get();
    }

    // VST2 only
    int canDo(const char *what) const override;
    intptr_t vendorSpecific(int index, intptr_t value, void *p, float opt) override;
protected:
    int numParameters() const { return info_->numParameters(); }
    int numPrograms() const { return info_->numPrograms(); }
    void pushCommand(const Command& cmd) override {
        commands_.push_back(cmd);
    }
    template<typename T>
    void doProcess(ProcessData& data);
    void sendCommands(RTChannel& channel);
    void dispatchReply(const ShmCommand &reply);

    IFactory::const_ptr factory_; // keep alive!
    PluginDesc::const_ptr info_;
    IWindow::ptr window_;
    IPluginListener* listener_ = nullptr;
    PluginBridge::ptr bridge_;
    uint32_t id_;
    std::vector<Command> commands_;
    int program_ = 0;
    int latency_ = 0;
    double transport_;
    // cache
    std::unique_ptr<std::atomic<float>[]> paramValueCache_;
    // use fixed sized arrays to avoid potential heap allocations with std::string
    // After all, parameter displays are typically rather short. If the string
    // happens to be larger than the array, we just truncate it.
    using ParamDisplay = std::array<char, 16>;
    std::unique_ptr<ParamDisplay[]> paramDisplayCache_;
    using ProgramName = std::array<char, 32>;
    std::unique_ptr<ProgramName[]> programNameCache_;
    // Normally, these are accessed on the same thread, so there would be
    // no contention. Notable exceptions: the plugin is multi-threaded
    // or we need to get the parameters from a different thread.
    // However, we always pay for the atomic operations...
    using ScopedLock = std::lock_guard<SpinLock>;
    mutable SpinLock cacheLock_;
    char padding[60];
};

class WindowClient : public IWindow {
 public:
    WindowClient(PluginClient &plugin);
    ~WindowClient();

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;

    void resize(int w, int h) override {
        // ignore
    }
 private:
    PluginClient *plugin_;
};

} // vst
