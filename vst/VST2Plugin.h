// VST2Plugin
#pragma once

#include "Interface.h"

#ifdef USE_FST
# define FST2VST
# include "fst/fst.h"
#else
//#include "aeffect.h"
# include "aeffectx.h"
// #include "vstfxstore.h"
#endif

namespace vst {

class VST2Factory : public IFactory {
 public:
    static VstInt32 shellPluginID;

    VST2Factory(const std::string& path);
    ~VST2Factory();
    // get a list of all available plugins
    void addPlugin(PluginInfo::ptr desc) override;
    PluginInfo::const_ptr getPlugin(int index) const override;
    int numPlugins() const override;
    // probe plugins (in a seperate process)
    void probe(ProbeCallback callback) override;
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
    using EntryPoint = AEffect *(*)(audioMasterCallback);
    std::string path_;
    std::unique_ptr<IModule> module_;
    EntryPoint entry_;
    std::vector<PluginInfo::ptr> plugins_;
    std::unordered_map<std::string, PluginInfo::ptr> pluginMap_;
    bool valid_ = false;
};

class VST2Plugin final : public IPlugin {
    friend class VST2Factory;
 public:
    static VstIntPtr VSTCALLBACK hostCallback(AEffect *plugin, VstInt32 opcode,
        VstInt32 index, VstIntPtr value, void *p, float opt);

    VST2Plugin(AEffect* plugin, IFactory::const_ptr f, PluginInfo::const_ptr desc);
    ~VST2Plugin();

    const PluginInfo& info() const override { return *desc_; }
    std::string getPluginName() const override;
    std::string getPluginVendor() const override;
    std::string getPluginCategory() const override;
    std::string getPluginVersion() const override;
    std::string getSDKVersion() const override;
    int getPluginUniqueID() const override;
    int canDo(const char *what) const override;
    intptr_t vendorSpecific(int index, intptr_t value, void *p, float opt) override;

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
    double getTransportPosition() const override {
        return timeInfo_.ppqPos;
    }

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
    static bool canHostDo(const char *what);

    bool hasFlag(VstAEffectFlags flag) const;
    void parameterAutomated(int index, float value);
    VstTimeInfo * getTimeInfo(VstInt32 flags);
    void preProcess(int nsamples);
    void postProcess(int nsample);
        // process VST events from plugin
    void processEvents(VstEvents *events);
        // dispatch to plugin
    VstIntPtr dispatch(VstInt32 opCode, VstInt32 index = 0, VstIntPtr value = 0,
        void *p = 0, float opt = 0) const;
        // data members
    VstIntPtr callback(VstInt32 opcode, VstInt32 index,
                           VstIntPtr value, void *ptr, float opt);
    AEffect *plugin_ = nullptr;
    IFactory::const_ptr factory_; // just to ensure lifetime
    PluginInfo::const_ptr desc_;
    IWindow::ptr window_;
    IPluginListener::ptr listener_ = nullptr;
    VstTimeInfo timeInfo_;
        // buffers for incoming MIDI and SysEx events
    std::vector<VstMidiEvent> midiQueue_;
    std::vector<VstMidiSysexEvent> sysexQueue_;
    VstEvents *vstEvents_; // VstEvents is basically an array of VstEvent pointers
    int vstEventBufferSize_ = 0;
    bool vstTimeWarned_ = false;
};

} // vst
