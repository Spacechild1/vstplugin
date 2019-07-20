#pragma once
#include "VSTPluginInterface.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"

#include <unordered_map>

using namespace Steinberg;

namespace vst {

class VST3Factory : public IFactory {
 public:
    VST3Factory(const std::string& path);
    ~VST3Factory();
    // get a list of all available plugins
    std::vector<std::shared_ptr<PluginInfo>> plugins() const override;
    int numPlugins() const override;
    // probe plugins (in a seperate process)
    void probe() override;
    bool isProbed() const override {
        return plugins_.size() > 0;
    }
    std::string path() const override {
        return path_;
    }
    // create a new plugin instance
    std::unique_ptr<IPlugin> create(const std::string& name, bool probe = false) const override;
 private:
    std::string path_;
    std::unique_ptr<IModule> module_;
    IPtr<IPluginFactory> factory_;
    // TODO dllExit
    std::vector<PluginInfoPtr> plugins_;
    std::unordered_map<std::string, int> nameMap_;
};

class VST3Plugin final : public IPlugin {
 public:
    VST3Plugin(IPtr<IPluginFactory> factory, int which, PluginInfoPtr desc);
    ~VST3Plugin();

    const PluginInfo& info() const { return *desc_; }
    std::string getPluginName() const override;
    std::string getPluginVendor() const override;
    std::string getPluginCategory() const override;
    std::string getPluginVersion() const override;
    std::string getSDKVersion() const override;
    int getPluginUniqueID() const override;
    virtual int canDo(const char *what) const override;
    virtual intptr_t vendorSpecific(int index, intptr_t value, void *ptr, float opt) override;

    void process(const float **inputs, float **outputs, int nsamples) override;
    void processDouble(const double **inputs, double **outputs, int nsamples) override;
    bool hasPrecision(ProcessPrecision precision) const override;
    void setPrecision(ProcessPrecision precision) override;
    void suspend() override;
    void resume() override;
    void setSampleRate(float sr) override;
    void setBlockSize(int n) override;
    int getNumInputs() const override;
    int getNumOutputs() const override;
    bool isSynth() const override;
    bool hasTail() const override;
    int getTailSize() const override;
    bool hasBypass() const override;
    void setBypass(bool bypass) override;
    void setNumSpeakers(int in, int out) override;

    void setListener(IPluginListener *listener) override {
        listener_ = listener;
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
    std::string getParameterName(int index) const override;
    std::string getParameterLabel(int index) const override;
    std::string getParameterDisplay(int index) const override;
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

    bool readProgramFile(const std::string& path) override;
    bool readProgramData(const char *data, size_t size) override;
    bool readProgramData(const std::string& buffer) override {
        return readProgramData(buffer.data(), buffer.size());
    }
    void writeProgramFile(const std::string& path) override;
    void writeProgramData(std::string& buffer) override;
    bool readBankFile(const std::string& path) override;
    bool readBankData(const char *data, size_t size) override;
    bool readBankData(const std::string& buffer) override {
        return readBankData(buffer.data(), buffer.size());
    }
    void writeBankFile(const std::string& path) override;
    void writeBankData(std::string& buffer) override;

    bool hasEditor() const override;
    void openEditor(void *window) override;
    void closeEditor() override;
    void getEditorRect(int &left, int &top, int &right, int &bottom) const override;
 private:
    std::string getBaseName() const;
    IPluginListener *listener_ = nullptr;
    PluginInfoPtr desc_;
    std::string name_;
    std::string vendor_;
    std::string version_;
    std::string sdkVersion_;
    std::string category_;
    IPtr<Vst::IComponent> component_;
    IPtr<Vst::IEditController> controller_;
};

} // vst
