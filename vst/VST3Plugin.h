#pragma once
#include "Interface.h"
#include "Utility.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstautomationstate.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/gui/iplugview.h"

#include <unordered_map>
#include <atomic>
#if HAVE_NRT_THREAD
#include <mutex>
#endif

namespace Steinberg {
namespace Vst {
// copied from public.sdk/vst/vstpresetfile.h
using ChunkID = char[4];

enum ChunkType
{
    kHeader,
    kComponentState,
    kControllerState,
    kProgramData,
    kMetaInfo,
    kChunkList,
    kNumPresetChunks
};

const ChunkID& getChunkID (ChunkType type);

} // Vst
} // Steinberg

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
    void doLoad();
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

//----------------------------------------------------------------------
class ParamValueQueue: public Vst::IParamValueQueue {
 public:
    static const int maxNumPoints = 64;

    ParamValueQueue();

    MY_IMPLEMENT_QUERYINTERFACE(Vst::IParamValueQueue)
    DUMMY_REFCOUNT_METHODS

    void setParameterId(Vst::ParamID id);
    Vst::ParamID PLUGIN_API getParameterId() override { return id_; }
    int32 PLUGIN_API getPointCount() override { return values_.size(); }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, Vst::ParamValue& value) override;
    tresult PLUGIN_API addPoint (int32 sampleOffset, Vst::ParamValue value, int32& index) override;
 protected:
    struct Value {
        Value(Vst::ParamValue v, int32 offset) : value(v), sampleOffset(offset) {}
        Vst::ParamValue value;
        int32 sampleOffset;
    };
    std::vector<Value> values_;
    Vst::ParamID id_ = Vst::kNoParamId;
};

//----------------------------------------------------------------------
class ParameterChanges: public Vst::IParameterChanges {
 public:
    MY_IMPLEMENT_QUERYINTERFACE(Vst::IParameterChanges)
    DUMMY_REFCOUNT_METHODS

    void setMaxNumParameters(int n){
        parameterChanges_.resize(n);
    }
    int32 PLUGIN_API getParameterCount() override {
        return useCount_;
    }
    Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override;
    Vst::IParamValueQueue* PLUGIN_API addParameterData(const Vst::ParamID& id, int32& index) override;
    void clear(){
        useCount_ = 0;
    }
 protected:
    std::vector<ParamValueQueue> parameterChanges_;
    int useCount_ = 0;
};

//--------------------------------------------------------------------------------

class EventList : public Vst::IEventList {
 public:
    static const int maxNumEvents = 64;

    EventList();
    ~EventList();

    MY_IMPLEMENT_QUERYINTERFACE(Vst::IEventList)
    DUMMY_REFCOUNT_METHODS

    int32 PLUGIN_API getEventCount() override;
    tresult PLUGIN_API getEvent(int32 index, Vst::Event& e) override;
    tresult PLUGIN_API addEvent (Vst::Event& e) override;
    void addSysexEvent(const SysexEvent& event);
    void clear();
 protected:
    std::vector<Vst::Event> events_;
    std::vector<std::string> sysexEvents_;
};

//--------------------------------------------------------------------------------------------------------

class VST3Plugin final :
        public IPlugin,
        public Vst::IComponentHandler,
        public Vst::IConnectionPoint
{
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

    // IConnectionPoint
    tresult PLUGIN_API connect(Vst::IConnectionPoint* other) override;
    tresult PLUGIN_API disconnect(Vst::IConnectionPoint* other) override;
    tresult PLUGIN_API notify(Vst::IMessage* message) override;

    PluginType getType() const override { return PluginType::VST3; }

    const PluginInfo& info() const override { return *info_; }

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
    void setBypass(Bypass state) override;
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

    void setParameter(int index, float value, int sampleOffset = 0) override;
    bool setParameter(int index, const std::string& str, int sampleOffset = 0) override;
    float getParameter(int index) const override;
    std::string getParameterString(int index) const override;
    int getNumParameters() const override;

    void setProgram(int program) override;
    void setProgramName(const std::string& name) override;
    int getProgram() const override;
    std::string getProgramName() const override;
    std::string getProgramNameIndexed(int index) const override;
    int getNumPrograms() const override;

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
    bool getEditorRect(int &left, int &top, int &right, int &bottom) const override;
    void updateEditor() override;
    void setWindow(IWindow::ptr window) override {
        window_ = std::move(window);
    }
    IWindow *getWindow() const override {
        return window_.get();
    }

    // VST3 only
    void beginMessage() override;
    void addInt(const char* id, int64_t value) override;
    void addFloat(const char* id, double value) override;
    void addString(const char* id, const char *value) override;
    void addString(const char* id, const std::string& value) override;
    void addBinary(const char* id, const char *data, size_t size) override;
    void endMessage() override;
 protected:
    template<typename T>
    void doProcess(ProcessData<T>& inData);
    void handleEvents();
    void updateAutomationState();
    void sendMessage(Vst::IMessage* msg);
    void doSetParameter(Vst::ParamID, float value, int32 sampleOffset = 0);
    void updateParamCache();
    IPtr<Vst::IComponent> component_;
    IPtr<Vst::IEditController> controller_;
    FUnknownPtr<Vst::IAudioProcessor> processor_;
    IFactory::const_ptr factory_;
    PluginInfo::const_ptr info_;
    IWindow::ptr window_;
    std::weak_ptr<IPluginListener> listener_;
    // audio
    enum BusType {
        Main = 0,
        Aux = 1
    };
    int numInputs_[2]; // main + aux
    int numOutputs_[2]; // main + aux
    Vst::ProcessContext context_;
    int32 automationState_ = 0;
    Bypass bypass_ = Bypass::Off;
    Bypass lastBypass_ = Bypass::Off;
    bool bypassSilent_ = false; // check if we can stop processing
    // midi
    EventList inputEvents_;
    EventList outputEvents_;
    int numMidiInChannels_ = 0;
    int numMidiOutChannels_ = 0;
    // parameters
    ParameterChanges inputParamChanges_;
    // ParameterChanges outputParamChanges_;
    std::vector<Vst::ParamValue> paramCache_;
    // programs
    int program_ = 0;
    // message from host to plugin
    IPtr<Vst::IMessage> msg_;
};

//--------------------------------------------------------------------------------

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

//--------------------------------------------------------------------------------

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

//-----------------------------------------------------------------------------

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

//-----------------------------------------------------------------------------

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

//-----------------------------------------------------------------------------

Vst::IHostApplication * getHostContext();

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

//-----------------------------------------------------------------------------

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

//-----------------------------------------------------------------------------

struct HostAttribute {
    enum Type
    {
        kInteger,
        kFloat,
        kString,
        kBinary
    };
    explicit HostAttribute(int64 value) : type(kInteger) { v.i = value; }
    explicit HostAttribute(double value) : type(kFloat) { v.f = value; }
    explicit HostAttribute(const Vst::TChar* s);
    explicit HostAttribute(const char * data, uint32 n);
    HostAttribute(const HostAttribute& other) = delete; // LATER
    HostAttribute(HostAttribute&& other);
    ~HostAttribute();
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

//-----------------------------------------------------------------------------

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

//-----------------------------------------------------------------------------

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

    void print();
protected:
    HostAttribute* find(AttrID aid);
    std::unordered_map<std::string, HostAttribute> list_;
};

//-----------------------------------------------------------------------------

class HostMessage : public HostObject, public Vst::IMessage {
public:
    MY_IMPLEMENT_QUERYINTERFACE(Vst::IMessage)
    MY_REFCOUNT_METHODS(HostObject)

    const char* PLUGIN_API getMessageID () override { return messageID_.c_str(); }
    void PLUGIN_API setMessageID (const char* messageID) override { messageID_ = messageID; }
    Vst::IAttributeList* PLUGIN_API getAttributes () override;

    void print();
protected:
    std::string messageID_;
    IPtr<HostAttributeList> attributes_;
};


} // vst
