// VST2Plugin
#pragma once

#include "Interface.h"

#if USE_FST
#include "fst.h"
#else
#define VST_FORCE_DEPRECATED 0
#include "aeffectx.h"
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
    ProbeFuture probeAsync() override;
    bool isProbed() const override {
        return !plugins_.empty();
    }
    bool valid() const override {
        return numPlugins() > 0;
    }
    std::string path() const override {
        return path_;
    }
    // create a new plugin instance
    IPlugin::ptr create(const std::string& name, bool probe = false) const override;
 private:
    void doLoad();
    using EntryPoint = AEffect *(*)(audioMasterCallback);
    std::string path_;
    std::unique_ptr<IModule> module_;
    EntryPoint entry_;
    std::vector<PluginInfo::ptr> plugins_;
    std::unordered_map<std::string, PluginInfo::ptr> pluginMap_;
};

//-----------------------------------------------------------------------------

class VST2Plugin final : public IPlugin {
    friend class VST2Factory;
 public:
    static VstIntPtr VSTCALLBACK hostCallback(AEffect *plugin, VstInt32 opcode,
        VstInt32 index, VstIntPtr value, void *p, float opt);

    VST2Plugin(AEffect* plugin, IFactory::const_ptr f, PluginInfo::const_ptr desc);
    ~VST2Plugin();

    PluginType getType() const override { return PluginType::VST2; }

    const PluginInfo& info() const override { return *info_; }

    int canDo(const char *what) const override;
    intptr_t vendorSpecific(int index, intptr_t value, void *p, float opt) override;

    void setupProcessing(double sampleRate, int maxBlockSize, ProcessPrecision precision) override;
    void process(ProcessData<float>& data) override;
    void process(ProcessData<double>& data) override;
    void suspend() override;
    void resume() override;
    void setBypass(Bypass state) override;
    void setNumSpeakers(int in, int out, int auxIn = 0, int auxOut = 0) override;

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
    void sendMidiEvent(const MidiEvent& event) override;
    void sendSysexEvent(const SysexEvent& event) override;

    void setParameter(int index, float value, int sampleOffset = 0) override;
    bool setParameter(int index, const std::string& str, int sampleOffset = 0) override;
    float getParameter(int index) const override;
    std::string getParameterString(int index) const override;

    void setProgram(int program) override;
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

    void openEditor(void *window) override;
    void closeEditor() override;
    bool getEditorRect(int &left, int &top, int &right, int &bottom) const override;
    void updateEditor() override;
    void checkEditorSize(int &width, int &height) const override;
    void resizeEditor(int width, int height) override;
    bool canResize() const override;

    void setWindow(IWindow::ptr window) override {
        window_ = std::move(window);
    }
    IWindow *getWindow() const override {
        return window_.get();
    }
 private:
    std::string getPluginName() const;
    std::string getPluginVendor() const;
    std::string getPluginCategory() const;
    std::string getPluginVersion() const;
    std::string getSDKVersion() const;
    std::string getParameterName(int index) const;
    std::string getParameterLabel(int index) const;
    int getNumInputs() const;
    int getNumOutputs() const;
    int getNumParameters() const;
    int getNumPrograms() const;
    bool hasEditor() const;
    bool hasPrecision(ProcessPrecision precision) const;
    bool isSynth() const;
    bool hasTail() const;
    int getTailSize() const;
    bool hasBypass() const;
    int getNumMidiInputChannels() const;
    int getNumMidiOutputChannels() const;
    bool hasMidiInput() const;
    bool hasMidiOutput() const;
        // other helpers
    static bool canHostDo(const char *what);
    bool hasFlag(VstAEffectFlags flag) const;
    void parameterAutomated(int index, float value);
    VstTimeInfo * getTimeInfo(VstInt32 flags);
    bool hasChunkData() const;
    void setProgramChunkData(const void *data, size_t size);
    void getProgramChunkData(void **data, size_t *size) const;
    void setBankChunkData(const void *data, size_t size);
    void getBankChunkData(void **data, size_t *size) const;
        // processing
    void preProcess(int nsamples);
    template<typename T, typename TProc>
    void doProcessing(ProcessData<T>& data, TProc processRoutine);
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
    PluginInfo::const_ptr info_;
    IWindow::ptr window_;
    std::weak_ptr<IPluginListener> listener_;
        // processing
    int numInputChannels_ = 0;
    int numOutputChannels_ = 0;
    VstTimeInfo timeInfo_;
    Bypass bypass_ = Bypass::Off;
    Bypass lastBypass_ = Bypass::Off;
    bool haveBypass_ = false;
    bool bypassSilent_ = false; // check if we can stop processing
        // buffers for incoming MIDI and SysEx events
    std::vector<VstMidiEvent> midiQueue_;
    std::vector<VstMidiSysexEvent> sysexQueue_;
    VstEvents *vstEvents_; // VstEvents is basically an array of VstEvent pointers
    int vstEventBufferSize_ = 0;
    bool vstTimeWarned_ = false;
    bool editor_ = false;
};

} // vst
