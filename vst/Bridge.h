#pragma once

#include "Interface.h"
#include "DeferredPlugin.h"

namespace vst {

class PluginClient final : public DeferredPlugin {
 public:
    PluginType getType() const override;

    PluginClient(IFactory::const_ptr f, PluginInfo::const_ptr desc);
    ~PluginClient();

    const PluginInfo& info() const override {
        return *info_;
    }

    void setupProcessing(double sampleRate, int maxBlockSize, ProcessPrecision precision) override;
    void process(ProcessData<float>& data) override;
    void process(ProcessData<double>& data) override;
    void suspend() override;
    void resume() override;
    void setBypass(Bypass state) override;
    void setNumSpeakers(int in, int out, int auxIn = 0, int auxOut = 0) override;

    void setListener(IPluginListener::ptr listener) override;

    double getTransportPosition() const override;

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
    void writeProgramFile(const std::string& path) override;
    void writeProgramData(std::string& buffer) override;
    void readBankFile(const std::string& path) override;
    void readBankData(const char *data, size_t size) override;
    void readBankData(const std::string& buffer) {
        readBankData(buffer.data(), buffer.size());
    }
    void writeBankFile(const std::string& path) override;
    void writeBankData(std::string& buffer) override;

    void openEditor(void *window) override;
    void closeEditor() override;
    bool getEditorRect(int &left, int &top, int &right, int &bottom) const override {
        return false;
    }

    void setWindow(std::unique_ptr<IWindow> window) override {
        window_ = std::move(window);
    }
    IWindow* getWindow() const override {
        return window_.get();
    }

    // VST2 only
    int canDo(const char *what) const;
    intptr_t vendorSpecific(int index, intptr_t value, void *p, float opt);
    // VST3 only
    void beginMessage();
    void addInt(const char* id, int64_t value);
    void addFloat(const char* id, double value);
    void addString(const char* id, const char *value);
    void addString(const char* id, const std::string& value);
    void addBinary(const char* id, const char *data, size_t size);
    void endMessage();
 protected:
    IFactory::const_ptr factory_; // just to ensure lifetime
    PluginInfo::const_ptr info_;
    IWindow::ptr window_;
    std::weak_ptr<IPluginListener> listener_;

    int numParameters() const { return info_->numParameters(); }
    int numPrograms() const { return info_->numPrograms(); }
    void pushCommand(const Command& cmd) override {}
};

class WindowClient : public IWindow {
 public:
    WindowClient();
    ~WindowClient();

    void* getHandle() override; // get system-specific handle to the window

    void setTitle(const std::string& title) override;

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;
    void update() override;
};

} //
