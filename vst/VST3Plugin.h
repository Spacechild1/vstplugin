#pragma once
#include "Interface.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/gui/iplugview.h"

#include <unordered_map>

using namespace Steinberg;

namespace vst {

class VST3Factory : public IFactory {
 public:
    VST3Factory(const std::string& path);
    ~VST3Factory();
    // get a list of all available plugins
    void addPlugin(PluginInfo::ptr desc) override;
    PluginInfo::const_ptr getPlugin(int index) const override;
    int numPlugins() const override;
    // probe plugins (in a seperate process)
    ProbeFuture probeAsync() override;
    bool isProbed() const override {
        return !plugins_.empty();
    }
    bool valid() const override {
        return valid_;
    }
    std::string path() const override {
        return path_;
    }
    // create a new plugin instance
    IPlugin::ptr create(const std::string& name, bool probe = false) const override;
 private:
    std::string path_;
    std::unique_ptr<IModule> module_;
    IPtr<IPluginFactory> factory_;
    // TODO dllExit
    // probed plugins:
    std::vector<PluginInfo::ptr> plugins_;
    std::unordered_map<std::string, PluginInfo::ptr> pluginMap_;
    // factory plugins:
    std::vector<std::string> pluginList_;
    mutable std::unordered_map<std::string, int> pluginIndexMap_;
    bool valid_ = false;
};

class VST3Plugin final : public IPlugin {
 public:
    VST3Plugin(IPtr<IPluginFactory> factory, int which, IFactory::const_ptr f, PluginInfo::const_ptr desc);
    ~VST3Plugin();

    const PluginInfo& info() const { return *info_; }

    virtual int canDo(const char *what) const override;
    virtual intptr_t vendorSpecific(int index, intptr_t value, void *ptr, float opt) override;

    void setupProcessing(double sampleRate, int maxBlockSize, ProcessPrecision precision) override;
    void process(ProcessData<float>& data) override;
    void process(ProcessData<double>& data) override;
    bool hasPrecision(ProcessPrecision precision) const override;
    void suspend() override;
    void resume() override;
    int getNumInputs() const override;
    virtual int getNumAuxInputs() const;
    virtual int getNumOutputs() const;
    virtual int getNumAuxOutputs() const;
    bool isSynth() const override;
    bool hasTail() const override;
    int getTailSize() const override;
    bool hasBypass() const override;
    void setBypass(bool bypass) override;
    void setNumSpeakers(int in, int out, int auxIn, int auxOut) override;

    void setListener(IPluginListener::ptr listener) override {
        listener_ = std::move(listener);
    }

    void setTempoBPM(double tempo) override;
    void setTimeSignature(int numerator, int denominator) override;
    void setTransportPlaying(bool play) override;
    void setTransportRecording(bool record) override;
    void setTransportAutomationWriting(bool writing) override;
    void setTransportAutomationReading(bool reading) override;
    void setTransportCycleActive(bool active) override;
    void setTransportCycleStart(double beat) override;
    void setTransportCycleEnd(double beat) override;
    void setTransportPosition(double beat) override;
    double getTransportPosition() const override;

    int getNumMidiInputChannels() const override;
    int getNumMidiOutputChannels() const override;
    bool hasMidiInput() const override;
    bool hasMidiOutput() const override;
    void sendMidiEvent(const MidiEvent& event) override;
    void sendSysexEvent(const SysexEvent& event) override;

    void setParameter(int index, float value) override;
    bool setParameter(int index, const std::string& str) override;
    float getParameter(int index) const override;
    std::string getParameterString(int index) const override;
    int getNumParameters() const override;

    void setProgram(int program) override;
    void setProgramName(const std::string& name) override;
    int getProgram() const override;
    std::string getProgramName() const override;
    std::string getProgramNameIndexed(int index) const override;
    int getNumPrograms() const override;

    bool hasChunkData() const override;
    void setProgramChunkData(const void *data, size_t size) override;
    void getProgramChunkData(void **data, size_t *size) const override;
    void setBankChunkData(const void *data, size_t size) override;
    void getBankChunkData(void **data, size_t *size) const override;

    void readProgramFile(const std::string& path) override;
    void readProgramData(const char *data, size_t size) override;
    void writeProgramFile(const std::string& path) override;
    void writeProgramData(std::string& buffer) override;
    void readBankFile(const std::string& path) override;
    void readBankData(const char *data, size_t size) override;
    void writeBankFile(const std::string& path) override;
    void writeBankData(std::string& buffer) override;

    bool hasEditor() const override;
    void openEditor(void *window) override;
    void closeEditor() override;
    void getEditorRect(int &left, int &top, int &right, int &bottom) const override;
    void setWindow(IWindow::ptr window) override {
        window_ = std::move(window);
    }
    IWindow *getWindow() const override {
        return window_.get();
    }
 private:
    IPtr<Vst::IComponent> component_;
    IPtr<Vst::IEditController> controller_;
    FUnknownPtr<Vst::IAudioProcessor> processor_;
    IFactory::const_ptr factory_;
    PluginInfo::const_ptr info_;
    IWindow::ptr window_;
    std::weak_ptr<IPluginListener> listener_;
    // busses (channel count + index)
    int numInputs_ = 0;
    int inputIndex_ = -1;
    int numAuxInputs_ = 0;
    int auxInputIndex_ = -1;
    int numOutputs_ = 0;
    int outputIndex_ = -1;
    int numAuxOutputs_ = 0;
    int auxOutputIndex_ = -1;
    int numMidiInChannels_ = 0;
    int midiInIndex_ = -1;
    int numMidiOutChannels_ = 0;
    int midiOutIndex_ = -1;
    // special parameters
    int programChangeID_ = -1;
    int bypassID_ = -1;
};

} // vst
