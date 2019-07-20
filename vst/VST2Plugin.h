// VST2Plugin
#pragma once

#include "VSTPluginInterface.h"

#ifdef USE_FST
# define FST2VST
# include "fst/fst.h"
#else
//#include "aeffect.h"
# include "aeffectx.h"
// #include "vstfxstore.h"
#endif

namespace vst {

class VST2Factory : public IVSTFactory {
 public:
    static VstInt32 shellPluginID;

    VST2Factory(const std::string& path);
    ~VST2Factory();
    // get a list of all available plugins
    void addPlugin(VSTPluginDesc::ptr desc) override;
    VSTPluginDesc::const_ptr getPlugin(int index) const override;
    int numPlugins() const override;
    // probe plugins (in a seperate process)
    void probe() override;
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
    IVSTPlugin::ptr create(const std::string& name, bool probe = false) const override;
 private:
    using EntryPoint = AEffect *(*)(audioMasterCallback);
    std::string path_;
    std::unique_ptr<IModule> module_;
    EntryPoint entry_;
    std::vector<VSTPluginDesc::ptr> plugins_;
    std::unordered_map<std::string, VSTPluginDesc::ptr> pluginMap_;
    bool valid_ = false;
};

class VST2Plugin final : public IVSTPlugin {
    friend class VST2Factory;
 public:
    static VstIntPtr VSTCALLBACK hostCallback(AEffect *plugin, VstInt32 opcode,
        VstInt32 index, VstIntPtr value, void *p, float opt);

    VST2Plugin(AEffect* plugin, IVSTFactory::const_ptr f, VSTPluginDesc::const_ptr desc);
    ~VST2Plugin();

    const VSTPluginDesc& info() const { return *desc_; }
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
    bool hasPrecision(VSTProcessPrecision precision) const override;
    void setPrecision(VSTProcessPrecision precision) override;
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

    void setListener(IVSTPluginListener::ptr listener) override {
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
    void sendMidiEvent(const VSTMidiEvent& event) override;
    void sendSysexEvent(const VSTSysexEvent& event) override;

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
    IVSTFactory::const_ptr factory_; // just to ensure lifetime
    VSTPluginDesc::const_ptr desc_;
    IVSTPluginListener::ptr listener_ = nullptr;
    VstTimeInfo timeInfo_;
        // buffers for incoming MIDI and SysEx events
    std::vector<VstMidiEvent> midiQueue_;
    std::vector<VstMidiSysexEvent> sysexQueue_;
    VstEvents *vstEvents_; // VstEvents is basically an array of VstEvent pointers
    int vstEventBufferSize_ = 0;
    bool vstTimeWarned_ = false;
};

} // vst
