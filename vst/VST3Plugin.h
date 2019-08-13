#pragma once
#include "Interface.h"
#include "Utility.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/ibstream.h"

#include "public.sdk/source/vst/vstpresetfile.h"
#include "public.sdk/source/vst/hosting/stringconvert.h"

#include <unordered_map>
#include <atomic>

using namespace Steinberg;

#define MY_IMPLEMENT_QUERYINTERFACE(Interface)                            \
tresult PLUGIN_API queryInterface (const TUID _iid, void** obj) override  \
{                                                                         \
    QUERY_INTERFACE (_iid, obj, FUnknown::iid, Interface)                 \
    QUERY_INTERFACE (_iid, obj, Interface::iid, Interface)                \
    *obj = nullptr;                                                       \
    return kNoInterface;                                                  \
}

#define DUMMY_REFCOUNT_METHODS                     \
uint32 PLUGIN_API addRef() override { return 1; }  \
uint32 PLUGIN_API release() override { return 1; } \


#define MY_REFCOUNT_METHODS(BaseClass) \
uint32 PLUGIN_API addRef() override { return BaseClass::addRef (); } \
uint32 PLUGIN_API release()override { return BaseClass::release (); }


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

class VST3Plugin final : public IPlugin, public Vst::IComponentHandler {
 public:
    VST3Plugin(IPtr<IPluginFactory> factory, int which, IFactory::const_ptr f, PluginInfo::const_ptr desc);
    ~VST3Plugin();

    MY_IMPLEMENT_QUERYINTERFACE(Vst::IComponentHandler)
    DUMMY_REFCOUNT_METHODS

    // IComponentHandler
    tresult PLUGIN_API beginEdit(Vst::ParamID id) override;
    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue value) override;
    tresult PLUGIN_API endEdit(Vst::ParamID id) override;
    tresult PLUGIN_API restartComponent(int32 flags) override;


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
    int getNumAuxInputs() const override;
    int getNumOutputs() const override;
    int getNumAuxOutputs() const override;
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
    TUID uid_;
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

class BaseStream : public IBStream {
 public:
    MY_IMPLEMENT_QUERYINTERFACE(IBStream)
    DUMMY_REFCOUNT_METHODS
    // IBStream
    tresult PLUGIN_API read  (void* buffer, int32 numBytes, int32* numBytesRead) override;
    tresult PLUGIN_API write (void* buffer, int32 numBytes, int32* numBytesWritten) override;
    tresult PLUGIN_API seek  (int64 pos, int32 mode, int64* result) override;
    tresult PLUGIN_API tell  (int64* pos) override;
    virtual const char *data() const = 0;
    virtual size_t size() const = 0;
    void setPos(int64 pos);
    int64 getPos() const;
    void rewind();
    bool writeInt32(int32 i);
    bool writeInt64(int64 i);
    bool writeChunkID(const Vst::ChunkID id);
    bool writeTUID(const TUID tuid);
    bool readInt32(int32& i);
    bool readInt64(int64& i);
    bool readChunkID(Vst::ChunkID id);
    bool readTUID(TUID tuid);
 protected:
    template<typename T>
    bool doWrite(const T& t);
    template<typename T>
    bool doRead(T& t);
    int64_t cursor_ = 0;
};

class ConstStream : public BaseStream {
 public:
    ConstStream() = default;
    ConstStream(const char *data, size_t size);
    void assign(const char *data, size_t size);
    // IBStream
    const char * data() const override { return data_; }
    size_t size() const override { return size_; }
 protected:
    const char *data_ = nullptr;
    int64_t size_ = 0;
};

class WriteStream : public BaseStream {
 public:
    WriteStream() = default;
    WriteStream(const char *data, size_t size);
    tresult PLUGIN_API write (void* buffer, int32 numBytes, int32* numBytesWritten) override;
    const char * data() const override { return buffer_.data(); }
    size_t size() const override { return buffer_.size(); }
    void transfer(std::string& dest);
protected:
    std::string buffer_;
};

Vst::IHostApplication * getHostContext();

struct FUID {
    FUID(){
        memset(uid, 0, sizeof(TUID));
    }
    FUID(const TUID _iid){
        memcpy(uid, _iid, sizeof(TUID));
    }
    bool operator==(const TUID _iid) const {
        return memcmp(uid, _iid, sizeof(TUID)) == 0;
    }
    bool operator!=(const TUID _iid) const {
        return !(*this == _iid);
    }
    TUID uid;
};

class PlugInterfaceSupport : public Vst::IPlugInterfaceSupport {
public:
    PlugInterfaceSupport ();
    MY_IMPLEMENT_QUERYINTERFACE(Vst::IPlugInterfaceSupport)
    DUMMY_REFCOUNT_METHODS

    //--- IPlugInterfaceSupport ---------
    tresult PLUGIN_API isPlugInterfaceSupported (const TUID _iid) override;
    void addInterface(const TUID _id);
private:
    std::vector<FUID> supportedInterfaces_;
};

class HostApplication : public Vst::IHostApplication {
public:
    HostApplication ();
    virtual ~HostApplication ();
    tresult PLUGIN_API queryInterface(const TUID _iid, void **obj) override;
    DUMMY_REFCOUNT_METHODS

    tresult PLUGIN_API getName (Vst::String128 name) override;
    tresult PLUGIN_API createInstance (TUID cid, TUID _iid, void** obj) override;
protected:
    std::unique_ptr<PlugInterfaceSupport> interfaceSupport_;
};

struct HostAttribute {
    enum Type
    {
        kInteger,
        kFloat,
        kString,
        kBinary
    };
    explicit HostAttribute(int64_t value) : type(kInteger) { v.i = value; }
    explicit HostAttribute(double value) : type(kFloat) { v.f = value; }
    explicit HostAttribute(const Vst::TChar* data, uint32 n) : size(n), type(kString){
        v.s = new Vst::TChar[size];
        memcpy(v.s, data, n * sizeof(Vst::TChar));
    }
    explicit HostAttribute(const char * data, uint32 n) : size(n), type(kBinary){
        v.b = new char[size];
        memcpy(v.s, data, n);
    }
    HostAttribute(const HostAttribute& other) = delete; // LATER
    HostAttribute(HostAttribute&& other){
        if (size > 0){
            delete[] v.b;
        }
        type = other.type;
        size = other.size;
        v = other.v;
        other.size = 0;
        other.v.b = nullptr;
    }
    ~HostAttribute(){
        if (size > 0){
            delete[] v.b;
        }
    }
    HostAttribute& operator =(const HostAttribute& other) = delete; // LATER
    HostAttribute& operator =(HostAttribute&& other) = delete; // LATER
    // data
    union v
    {
      int64 i;
      double f;
      Vst::TChar* s;
      char* b;
    } v;
    uint32 size = 0;
    Type type;
};

class HostObject : public FUnknown {
public:
    virtual ~HostObject() {}

    uint32 PLUGIN_API addRef() override {
        return ++refcount_;
    }
    uint32 PLUGIN_API release() override {
        auto res = --refcount_;
        if (res == 0){
            delete this;
        }
        return res;
    }
private:
    std::atomic<int32_t> refcount_{1};
};

class HostAttributeList : public HostObject, public Vst::IAttributeList {
public:
    MY_IMPLEMENT_QUERYINTERFACE(Vst::IAttributeList)
    MY_REFCOUNT_METHODS(HostObject)

    tresult PLUGIN_API setInt (AttrID aid, int64 value) override;
    tresult PLUGIN_API getInt (AttrID aid, int64& value) override;
    tresult PLUGIN_API setFloat (AttrID aid, double value) override;
    tresult PLUGIN_API getFloat (AttrID aid, double& value) override;
    tresult PLUGIN_API setString (AttrID aid, const Vst::TChar* string) override;
    tresult PLUGIN_API getString (AttrID aid, Vst::TChar* string, uint32 size) override;
    tresult PLUGIN_API setBinary (AttrID aid, const void* data, uint32 size) override;
    tresult PLUGIN_API getBinary (AttrID aid, const void*& data, uint32& size) override;
protected:
    HostAttribute* find(AttrID aid);
    std::unordered_map<std::string, HostAttribute> list_;
};

class HostMessage : public HostObject, public Vst::IMessage {
public:
    MY_IMPLEMENT_QUERYINTERFACE(Vst::IMessage)
    MY_REFCOUNT_METHODS(HostObject)

    const char* PLUGIN_API getMessageID () override { return messageID_.c_str(); }
    void PLUGIN_API setMessageID (const char* messageID) override { messageID_ = messageID; }
    Vst::IAttributeList* PLUGIN_API getAttributes () override {
        LOG_DEBUG("HostMessage::getAttributes");
        if (!attributeList_){
            attributeList_.reset(new HostAttributeList);
        }
        return attributeList_.get();
    }
protected:
    std::string messageID_;
    IPtr<HostAttributeList> attributeList_;
};


} // vst
