#pragma once

#include "Interface.h"
#include "DeferredPlugin.h"
#include "PluginBridge.h"

namespace vst {

class PluginClient final : public DeferredPlugin {
 public:
    PluginClient(IFactory::const_ptr f, PluginInfo::const_ptr desc, bool sandbox);
    virtual ~PluginClient();

    const PluginInfo& info() const override {
        return *info_;
    }

    PluginBridge& bridge() {
        return *bridge_;
    }

    bool check();

    uint32_t id() const { return id_; }

    void setupProcessing(double sampleRate, int maxBlockSize, ProcessPrecision precision) override;
    void process(ProcessData<float>& data) override;
    void process(ProcessData<double>& data) override;
    void suspend() override;
    void resume() override;
    void setNumSpeakers(int in, int out, int auxIn = 0, int auxOut = 0) override;
    int getLatencySamples() override;

    void setListener(IPluginListener::ptr listener) override;

    double getTransportPosition() const override;

    void setParameter(int index, float value, int sampleOffset) override;
    bool setParameter(int index, const std::string& str, int sampleOffset) override;
    float getParameter(int index) const override;
    std::string getParameterString(int index) const override;

    void setProgramName(const std::string& name) override;
    int getProgram() const override;
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
    void sendData(Command::Type type, const char *data, size_t size);

    void writeProgramFile(const std::string& path) override;
    void writeProgramData(std::string& buffer) override;
    void writeBankFile(const std::string& path) override;
    void writeBankData(std::string& buffer) override;

    void receiveData(Command::Type type, std::string& buffer);

    void openEditor(void *window) override;
    void closeEditor() override;
    bool getEditorRect(int &left, int &top, int &right, int &bottom) const override;
    void updateEditor() override;
    void checkEditorSize(int& width, int& height) const override;
    void resizeEditor(int width, int height) override;
    bool canResize() const override;

    void setWindow(std::unique_ptr<IWindow> window) override {
        window_ = std::move(window);
    }

    IWindow* getWindow() const override {
        return window_.get();
    }

    // VST2 only
    int canDo(const char *what) const override;
    intptr_t vendorSpecific(int index, intptr_t value, void *p, float opt) override;
    // VST3 only
    void beginMessage() override;
    void addInt(const char* id, int64_t value) override;
    void addFloat(const char* id, double value) override;
    void addString(const char* id, const char *value) override;
    void addString(const char* id, const std::string& value) override;
    void addBinary(const char* id, const char *data, size_t size) override;
    void endMessage() override;
 protected:
    IFactory::const_ptr factory_; // just to ensure lifetime
    PluginInfo::const_ptr info_;
    IWindow::ptr window_;
    std::weak_ptr<IPluginListener> listener_;
    PluginBridge::ptr bridge_;
    uint32_t id_;
    bool crashed_ = false;
    std::vector<Command> commands_;
    // cache
    struct Param {
        float value;
        std::string display;
    };
    std::unique_ptr<Param[]> paramCache_;
    std::unique_ptr<std::string[]> programCache_;
    int program_ = 0;
    int latency_ = 0;
    double transport_;

    int numParameters() const { return info_->numParameters(); }
    int numPrograms() const { return info_->numPrograms(); }
    void pushCommand(const Command& cmd) override {
        commands_.push_back(cmd);
    }
    template<typename T>
    void doProcess(ProcessData<T>& data);
    void sendCommands(RTChannel& channel);
    void dispatchReply(const ShmReply &reply);
};

class WindowClient : public IWindow {
 public:
    WindowClient(PluginClient &plugin);
    ~WindowClient();

    void* getHandle() override; // get system-specific handle to the window

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;
    void update() override;
 private:
    PluginClient *plugin_;
};

} // vst
