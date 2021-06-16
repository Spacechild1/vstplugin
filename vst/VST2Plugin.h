// VST2Plugin
#pragma once

#include "Interface.h"
#include "PluginFactory.h"

#define VST_FORCE_DEPRECATED 0
#include "aeffectx.h"

namespace vst {

class VST2Plugin;

class VST2Factory final : public PluginFactory {
 public:
    static VstInt32 shellPluginID;

    VST2Factory(const std::string& path, bool probe);
    ~VST2Factory();
    // probe a single plugin
    PluginDesc::const_ptr probePlugin(int id) const override;
    // create a new plugin instance
    IPlugin::ptr create(const std::string& name) const override;
 private:
    void doLoad();
    std::unique_ptr<VST2Plugin> doCreate(PluginDesc::const_ptr desc) const;
    // Although calling convention specifiers like __cdecl and __stdcall
    // should be meaningless on x86-64 platforms, Wine apparantely treats
    // them as a hint that a function (pointer) uses the Microsoft x64 calling
    // convention instead of System V! I couldn't find any documentation
    // to confirm this, but it seems to work in practice. Otherwise,
    // calling the entry point would crash immediately with stack corruption.
    using EntryPoint = AEffect *(VSTCALLBACK *)(audioMasterCallback);
    EntryPoint entry_;
};

//-----------------------------------------------------------------------------

class VST2Plugin final : public IPlugin {
    friend class VST2Factory;
 public:
    static VstIntPtr VSTCALLBACK hostCallback(AEffect *plugin, VstInt32 opcode,
        VstInt32 index, VstIntPtr value, void *p, float opt);

    VST2Plugin(AEffect* plugin, IFactory::const_ptr f, PluginDesc::const_ptr desc);
    ~VST2Plugin();

    const PluginDesc& info() const override { return *info_; }
    PluginDesc::const_ptr getInfo() const { return info_; }

    int canDo(const char *what) const override;
    intptr_t vendorSpecific(int index, intptr_t value, void *p, float opt) override;

    void setupProcessing(double sampleRate, int maxBlockSize,
                         ProcessPrecision precision, ProcessMode mode) override;
    void process(ProcessData& data) override;
    void suspend() override;
    void resume() override;
    void setBypass(Bypass state) override;
    void setNumSpeakers(int *input, int numInputs,
                        int *output, int numOutputs) override;
    int getLatencySamples() override;

    void setListener(IPluginListener::ptr listener) override {
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
    bool getEditorRect(Rect& rect) const override;
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
    void checkLatency();
    VstTimeInfo * getTimeInfo(VstInt32 flags);
    bool hasChunkData() const;
    void setProgramChunkData(const void *data, size_t size);
    void getProgramChunkData(void **data, size_t *size) const;
    void setBankChunkData(const void *data, size_t size);
    void getBankChunkData(void **data, size_t *size) const;
    // processing
    void preProcess(int nsamples);
    template<typename T, typename TProc>
    void doProcess(ProcessData& data, TProc processRoutine);
    template<typename T, typename TProc>
    void bypassProcess(ProcessData& data, TProc processRoutine,
                       Bypass state, bool ramp);
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
    PluginDesc::const_ptr info_;
    IWindow::ptr window_;
    std::weak_ptr<IPluginListener> listener_;
    // processing
    int latency_ = 0;
    ProcessMode mode_ = ProcessMode::Realtime;
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
    bool editor_ = false;
};

} // vst
