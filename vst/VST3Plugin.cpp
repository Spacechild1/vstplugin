#include "VST3Plugin.h"

#include <cstring>
#include <algorithm>
#include <set>
#include <codecvt>
#include <locale>

DEF_CLASS_IID (FUnknown)
DEF_CLASS_IID (IBStream)
DEF_CLASS_IID (IPlugFrame)
DEF_CLASS_IID (IPlugView)
DEF_CLASS_IID (IPluginBase)
DEF_CLASS_IID (IPluginFactory)
DEF_CLASS_IID (IPluginFactory2)
DEF_CLASS_IID (IPluginFactory3)
DEF_CLASS_IID (Vst::IHostApplication)
DEF_CLASS_IID (Vst::IPlugInterfaceSupport)
DEF_CLASS_IID (Vst::IAttributeList)
DEF_CLASS_IID (Vst::IEventList)
DEF_CLASS_IID (Vst::IParameterChanges)
DEF_CLASS_IID (Vst::IParamValueQueue)
DEF_CLASS_IID (Vst::IMessage)
DEF_CLASS_IID (Vst::IComponent)
DEF_CLASS_IID (Vst::IComponentHandler)
DEF_CLASS_IID (Vst::IConnectionPoint)
DEF_CLASS_IID (Vst::IEditController)
DEF_CLASS_IID (Vst::IAutomationState)
DEF_CLASS_IID (Vst::IMidiMapping)
DEF_CLASS_IID (Vst::IAudioProcessor)
DEF_CLASS_IID (Vst::IUnitInfo)
DEF_CLASS_IID (Vst::IUnitData)
DEF_CLASS_IID (Vst::IProgramListData)

namespace Steinberg {
namespace Vst {

//------------------------------------------------------------------------
// copied from public.sdk/vst/vstpresetfile.cpp

static const Vst::ChunkID commonChunks[Vst::kNumPresetChunks] = {
    {'V', 'S', 'T', '3'},	// kHeader
    {'C', 'o', 'm', 'p'},	// kComponentState
    {'C', 'o', 'n', 't'},	// kControllerState
    {'P', 'r', 'o', 'g'},	// kProgramData
    {'I', 'n', 'f', 'o'},	// kMetaInfo
    {'L', 'i', 's', 't'}	// kChunkList
};

// Preset Header: header id + version + class id + list offset
static const int32 kFormatVersion = 1;
static const int32 kClassIDSize = 32; // ASCII-encoded FUID
static const int32 kHeaderSize = sizeof (Vst::ChunkID) + sizeof (int32) + kClassIDSize + sizeof (TSize);
static const int32 kListOffsetPos = kHeaderSize - sizeof (TSize);

const Vst::ChunkID& getChunkID (Vst::ChunkType type)
{
    return commonChunks[type];
}

} // Vst
} // Steinberg


namespace vst {

#ifdef _WIN32
using unichar = wchar_t;
#else
using unichar = char16_t;
#endif

using StringCoverter = std::wstring_convert<std::codecvt_utf8_utf16<unichar>, unichar>;

static StringCoverter& stringConverter(){
#ifdef __MINGW32__
    // because of a mingw32 bug, destructors of thread_local STL objects segfault...
    thread_local auto conv = new StringCoverter;
    return *conv;
#else
    thread_local StringCoverter conv;
    return conv;
#endif
}

std::string convertString (const Vst::String128 str){
    return stringConverter().to_bytes(reinterpret_cast<const unichar *>(str));
}

bool convertString (const std::string& src, Steinberg::Vst::String128 dst){
    if (src.size() < 128){
        auto wstr = stringConverter().from_bytes(src);
        for (int i = 0; i < (int)wstr.size(); ++i){
            dst[i] = wstr[i];
        }
        dst[src.size()] = 0;
        return true;
    } else {
        return false;
    }
}

/*/////////////////////// VST3Factory /////////////////////////*/

VST3Factory::VST3Factory(const std::string& path)
    : path_(path) {}

VST3Factory::~VST3Factory(){
    factory_ = nullptr;
#if 0
    // this crashes on macOS when called during program termination
    if (module_ && !module_->exit()){
        // don't throw!
        LOG_ERROR("couldn't exit module");
    }
#endif
    // LOG_DEBUG("freed VST3 module " << path_);
}

void VST3Factory::addPlugin(PluginInfo::ptr desc){
    if (!pluginMap_.count(desc->name)){
        plugins_.push_back(desc);
        pluginMap_[desc->name] = desc;
    }
}

PluginInfo::const_ptr VST3Factory::getPlugin(int index) const {
    if (index >= 0 && index < (int)plugins_.size()){
        return plugins_[index];
    } else {
        return nullptr;
    }
}

PluginInfo::const_ptr VST3Factory::findPlugin(const std::string& name) const {
    auto it = pluginMap_.find(name);
    if (it != pluginMap_.end()){
        return it->second;
    } else {
        return nullptr;
    }
}

int VST3Factory::numPlugins() const {
    return plugins_.size();
}

IFactory::ProbeFuture VST3Factory::probeAsync() {
    doLoad(); // lazy loading

    if (pluginList_.empty()){
        throw Error(Error::ModuleError, "Factory doesn't have any plugin(s)");
    }
    plugins_.clear();
    pluginMap_.clear();
    auto self(shared_from_this());
    if (pluginList_.size() > 1){
        return [this, self=std::move(self)](ProbeCallback callback){
            ProbeList pluginList;
            for (auto& name : pluginList_){
                pluginList.emplace_back(name, 0);
            }
            plugins_ = probePlugins(pluginList, callback);
            for (auto& desc : plugins_) {
                pluginMap_[desc->name] = desc;
            }
        };
    } else {
        auto f = probePlugin(pluginList_[0]);
        return [this, self=std::move(self), f=std::move(f)](ProbeCallback callback){
            auto result = f();
            if (result.valid()) {
                plugins_ = { result.plugin };
                pluginMap_[result.plugin->name] = result.plugin;
            }
            /// LOG_DEBUG("probed plugin " << result.plugin->name);
            if (callback){
                callback(result);
            }
        };
    }
}

void VST3Factory::doLoad(){
    if (!module_){
        std::string modulePath = path_;
    #ifndef __APPLE__
        if (isDirectory(modulePath)){
        #ifdef _WIN32
            modulePath += "/" + getBundleBinaryPath() + "/" + fileName(path_);
        #else
            modulePath += "/" + getBundleBinaryPath() + "/" + fileBaseName(path_) + ".so";
        #endif
        }
    #endif
        auto module = IModule::load(modulePath); // throws on failure
        auto factoryProc = module->getFnPtr<GetFactoryProc>("GetPluginFactory");
        if (!factoryProc){
            throw Error(Error::ModuleError, "Couldn't find entry point (not a VST3 plugin?)");
        }
        if (!module->init()){
            throw Error(Error::ModuleError, "Couldn't init module");
        }
        factory_ = IPtr<IPluginFactory>(factoryProc());
        if (!factory_){
            throw Error(Error::ModuleError, "Couldn't get plugin factory");
        }
        /// LOG_DEBUG("VST3Factory: loaded " << path_);
        // map plugin names to indices
        auto numPlugins = factory_->countClasses();
        /// LOG_DEBUG("module contains " << numPlugins << " classes");
        pluginList_.clear();
        pluginIndexMap_.clear();
        for (int i = 0; i < numPlugins; ++i){
            PClassInfo ci;
            if (factory_->getClassInfo(i, &ci) == kResultTrue){
                /// LOG_DEBUG("\t" << ci.name << ", " << ci.category);
                if (!strcmp(ci.category, kVstAudioEffectClass)){
                    pluginList_.push_back(ci.name);
                    pluginIndexMap_[ci.name] = i;
                }
            } else {
                throw Error(Error::ModuleError, "Couldn't get class info!");
            }
        }
        // done
        module_ = std::move(module);
    }
}

std::unique_ptr<IPlugin> VST3Factory::create(const std::string& name, bool probe) const {
    const_cast<VST3Factory *>(this)->doLoad(); // lazy loading

    PluginInfo::ptr desc = nullptr; // will stay nullptr when probing!
    if (!probe){
        if (plugins_.empty()){
            throw Error(Error::ModuleError, "Factory doesn't have any plugin(s)");
        }
        auto it = pluginMap_.find(name);
        if (it == pluginMap_.end()){
            throw Error(Error::ModuleError, "Can't find (sub)plugin '" + name + "'");
        }
        desc = it->second;
    }
    return std::make_unique<VST3Plugin>(factory_, pluginIndexMap_[name], shared_from_this(), desc);
}

/*///////////////////// ParamValueQeue /////////////////////*/

ParamValueQueue::ParamValueQueue() {
    values_.reserve(maxNumPoints);
}

void ParamValueQueue::setParameterId(Vst::ParamID id){
    values_.clear();
    id_ = id;
}

tresult PLUGIN_API ParamValueQueue::getPoint(int32 index, int32& sampleOffset, Vst::ParamValue& value) {
    if (index >= 0 && index < (int32)values_.size()){
        auto& v = values_[index];
        value = v.value;
        sampleOffset = v.sampleOffset;
        return kResultTrue;
    }
    return kResultFalse;
}
tresult PLUGIN_API ParamValueQueue::addPoint (int32 sampleOffset, Vst::ParamValue value, int32& index) {
    // iterate in reverse because we likely add values in "chronological" order
    for (auto it = values_.end(); it-- != values_.begin(); ){
        if (sampleOffset > it->sampleOffset){
            // higher sample offset -> insert *after* this point (might actually append)
            if (values_.size() < maxNumPoints){
                // insert
                it = values_.emplace(it + 1, value, sampleOffset);
                index = it - values_.begin();
            } else {
                // replace
                values_.back() = Value(value, sampleOffset);
                index = values_.size() - 1;
            }
            return kResultOk;
        } else if (sampleOffset == it->sampleOffset){
            // equal sample offset -> replace point
            it->value = value;
            index = it - values_.begin();
            return kResultOk;
        }
    }
    // empty queue or smallest sample offset:
    if (values_.size() < maxNumPoints){
        // prepend point
        values_.emplace(values_.begin(), value, sampleOffset);
    } else {
        // replace first point
        values_.front() = Value(value, sampleOffset);
    }
    index = 0;
    return kResultOk;
}

/*///////////////////// ParameterChanges /////////////////////*/

Vst::IParamValueQueue* PLUGIN_API ParameterChanges::getParameterData(int32 index) {
    if (index >= 0 && index < useCount_){
        return &parameterChanges_[index];
    } else {
        return nullptr;
    }
}
Vst::IParamValueQueue* PLUGIN_API ParameterChanges::addParameterData(const Vst::ParamID& id, int32& index) {
    for (int i = 0; i < useCount_; ++i){
        auto& param = parameterChanges_[i];
        if (param.getParameterId() == id){
            index = i;
            return &param;
        }
    }
    if (useCount_ < (int)parameterChanges_.size()){
        index = useCount_++;
        parameterChanges_[index].setParameterId(id);
        return &parameterChanges_[index];
    } else {
        LOG_ERROR("bug addParameterData");
        index = 0;
        return nullptr;
    }
}

/*///////////////////// EventList /////////////////////*/

EventList::EventList(){
    events_.reserve(maxNumEvents);
}

EventList::~EventList() {}

int32 PLUGIN_API EventList::getEventCount() {
    return events_.size();
}

tresult PLUGIN_API EventList::getEvent(int32 index, Vst::Event& e) {
    if (index >= 0 && index < (int32)events_.size()){
        e = events_[index];
        return kResultOk;
    } else {
        return kResultFalse;
    }
}

tresult PLUGIN_API EventList::addEvent (Vst::Event& e) {
    // let's grow the queue beyond the limit...
    // LATER use a realtime allocator
    events_.push_back(e);
    return kResultOk;
}

void EventList::addSysexEvent(const SysexEvent& event){
    // this will allocate memory anyway...
    sysexEvents_.emplace_back(event.data, event.size);
    auto& last = sysexEvents_.back();
    Vst::Event e;
    memset(&e, 0, sizeof(Vst::Event));
    e.type = Vst::Event::kDataEvent;
    e.data.type = Vst::DataEvent::kMidiSysEx;
    e.data.bytes = (const uint8 *)last.data();
    e.data.size = last.size();
    addEvent(e);
}

void EventList::clear(){
    events_.clear();
    sysexEvents_.clear();
}

/*/////////////////////// VST3Plugin ///////////////////////*/

template <typename T>
inline IPtr<T> createInstance (IPtr<IPluginFactory> factory, TUID iid){
    T* obj = nullptr;
    if (factory->createInstance (iid, T::iid, reinterpret_cast<void**> (&obj)) == kResultTrue){
        return owned(obj);
    } else {
        return nullptr;
    }
}

VST3Plugin::VST3Plugin(IPtr<IPluginFactory> factory, int which, IFactory::const_ptr f, PluginInfo::const_ptr desc)
    : info_(std::move(desc))
{
    memset(&context_, 0, sizeof(context_));
    context_.state = Vst::ProcessContext::kContTimeValid
            | Vst::ProcessContext::kProjectTimeMusicValid | Vst::ProcessContext::kBarPositionValid
            | Vst::ProcessContext::kCycleValid | Vst::ProcessContext::kTempoValid
            | Vst::ProcessContext::kTimeSigValid | Vst::ProcessContext::kClockValid
            | Vst::ProcessContext::kSmpteValid;
    context_.sampleRate = 1;
    context_.tempo = 120;
    context_.timeSigNumerator = 4;
    context_.timeSigDenominator = 4;
    context_.frameRate.framesPerSecond = 60; // just pick one

    // are we probing?
    auto info = !info_ ? std::make_shared<PluginInfo>(f) : nullptr;
    TUID uid;
    PClassInfo2 ci2;
    auto factory2 = FUnknownPtr<IPluginFactory2> (factory);
    if (factory2 && factory2->getClassInfo2(which, &ci2) == kResultTrue){
        memcpy(uid, ci2.cid, sizeof(TUID));
        if (info){
            info->name = ci2.name;
            info->category = ci2.subCategories;
            info->vendor = ci2.vendor;
            info->version = ci2.version;
            info->sdkVersion = ci2.sdkVersion;
        }
    } else {
        Steinberg::PClassInfo ci;
        if (factory->getClassInfo(which, &ci) == kResultTrue){
            memcpy(uid, ci.cid, sizeof(TUID));
            if (info){
                info->name = ci.name;
                info->category = "Uncategorized";
                info->version = "0.0.0";
                info->sdkVersion = "VST 3";
            }
        } else {
            throw Error(Error::PluginError, "Couldn't get class info!");
        }
    }
    // create component
    if (!(component_ = createInstance<Vst::IComponent>(factory, uid))){
        throw Error(Error::PluginError, "Couldn't create VST3 component");
    }
    LOG_DEBUG("created VST3 component");
    // initialize component
    if (component_->initialize(getHostContext()) != kResultOk){
        throw Error(Error::PluginError, "Couldn't initialize VST3 component");
    }
    // first try to get controller from the component part (simple plugins)
    auto controller = FUnknownPtr<Vst::IEditController>(component_);
    if (controller){
        controller_ = shared(controller.getInterface());
    } else {
        // if this fails, try to instantiate controller class and initialize it
        TUID controllerCID;
        if (component_->getControllerClassId(controllerCID) == kResultTrue){
            controller_ = createInstance<Vst::IEditController>(factory, controllerCID);
            if (controller_ && (controller_->initialize(getHostContext()) != kResultOk)){
                throw Error(Error::PluginError, "Couldn't initialize VST3 controller");
            }
        }
    }
    if (controller_){
        LOG_DEBUG("created VST3 controller");
    } else {
        throw Error(Error::PluginError, "Couldn't get VST3 controller!");
    }
    if (controller_->setComponentHandler(this) != kResultOk){
        throw Error(Error::PluginError, "Couldn't set component handler");
    }
    FUnknownPtr<Vst::IConnectionPoint> componentCP(component_);
    FUnknownPtr<Vst::IConnectionPoint> controllerCP(controller_);
    // connect component and controller
    if (componentCP && controllerCP){
    #if 1
        // connect directly (not recommended)
        componentCP->connect(controllerCP);
        controllerCP->connect(componentCP);
    #else
        // use the host as a proxy.
        // actually, we would need seperate connection points for
        // the component and controller, so that we know the message destination.
        componentCP->connect(this);
        controllerCP->connect(this);
    #endif
        LOG_DEBUG("connected component and controller");
    }
    // synchronize state
    WriteStream stream;
    if (component_->getState(&stream) == kResultTrue){
        stream.rewind();
        if (controller_->setComponentState(&stream) == kResultTrue){
            LOG_DEBUG("synchronized state");
        } else {
            LOG_DEBUG("didn't synchronize state");
        }
    }
    // check processor
    if (!(processor_ = FUnknownPtr<Vst::IAudioProcessor>(component_))){
        throw Error(Error::PluginError, "Couldn't get VST3 processor");
    }
    // get IO channel count
    auto getChannelCount = [this](auto media, auto dir, auto type) -> int {
        auto count = component_->getBusCount(media, dir);
        for (int i = 0; i < count; ++i){
            Vst::BusInfo bus;
            if (component_->getBusInfo(media, dir, i, bus) == kResultTrue
                && bus.busType == type)
            // we assume that the first bus is 'main' and the second bus is 'aux'
            {
                if ((type == Vst::kMain && i != 0) ||
                    (type == Vst::kAux && i != 1))
                {
                    LOG_WARNING("unexpected bus index");
                    return 0;
                }
                return bus.channelCount;
            }
        }
        return 0;
    };
    // finally set remaining info
    if (info){
        info->setUID(uid);
        // vendor name (if still empty)
        if (info->vendor.empty()){
            PFactoryInfo i;
            if (factory->getFactoryInfo(&i) == kResultTrue){
                info->vendor = i.vendor;
            } else {
                info->vendor = "Unknown";
            }
        }
        info->numInputs = getChannelCount(Vst::kAudio, Vst::kInput, Vst::kMain);
        info->numAuxInputs = getChannelCount(Vst::kAudio, Vst::kInput, Vst::kAux);
        info->numOutputs = getChannelCount(Vst::kAudio, Vst::kOutput, Vst::kMain);
        info->numAuxOutputs = getChannelCount(Vst::kAudio, Vst::kOutput, Vst::kAux);
        uint32_t flags = 0;
        LOG_DEBUG("has editor: " << hasEditor());
        flags |= hasEditor();
        flags |= (info->category.find(Vst::PlugType::kInstrument) != std::string::npos) * PluginInfo::IsSynth;
        flags |= hasPrecision(ProcessPrecision::Single) * PluginInfo::SinglePrecision;
        flags |= hasPrecision(ProcessPrecision::Double) * PluginInfo::DoublePrecision;
        flags |= hasMidiInput() * PluginInfo::MidiInput;
        flags |= hasMidiOutput() * PluginInfo::MidiOutput;
        info->flags = flags;
        // get parameters
        std::set<Vst::ParamID> params;
        int numParameters = controller_->getParameterCount();
        for (int i = 0; i < numParameters; ++i){
            PluginInfo::Param param;
            Vst::ParameterInfo pi;
            if (controller_->getParameterInfo(i, pi) == kResultTrue){
                // some plugins have duplicate parameters... why?
                if (params.count(pi.id)){
                    continue;
                }
                param.name = convertString(pi.title);
                param.label = convertString(pi.units);
                param.id = pi.id;
                if (pi.flags & Vst::ParameterInfo::kIsProgramChange){
                    info->programChange = pi.id;
                } else if (pi.flags & Vst::ParameterInfo::kIsBypass){
                    info->bypass = pi.id;
                } else {
                    // Only show automatable parameters. This should hide MIDI CC parameters.
                    // Some JUCE plugins add thousands of (automatable) MIDI CC parameters,
                    // e.g. "MIDI CC 0|0" etc., so we need the following hack:
                    if ((pi.flags & Vst::ParameterInfo::kCanAutomate) &&
                            param.name.find("MIDI CC ") == std::string::npos)
                    {
                        params.insert(param.id);
                        info->addParameter(std::move(param));
                    }
                }
            } else {
                LOG_ERROR("couldn't get parameter info!");
            }
        }
        // programs
        auto ui = FUnknownPtr<Vst::IUnitInfo>(controller_);
        if (ui){
            int count = ui->getProgramListCount();
            if (count > 0){
                if (count > 1){
                    LOG_DEBUG("more than 1 program list!");
                }
                Vst::ProgramListInfo pli;
                if (ui->getProgramListInfo(0, pli) == kResultTrue){
                    for (int i = 0; i < pli.programCount; ++i){
                        Vst::String128 name;
                        if (ui->getProgramName(pli.id, i, name) == kResultTrue){
                            info->programs.push_back(convertString(name));
                        } else {
                            LOG_ERROR("couldn't get program name!");
                            info->programs.push_back("");
                        }
                    }
                    LOG_DEBUG("num programs: " << pli.programCount);
                } else {
                    LOG_ERROR("couldn't get program list info");
                }
            } else {
                LOG_DEBUG("no program list");
            }
        } else {
            LOG_DEBUG("no unit info");
        }
        info_ = info;
    }
    numMidiInChannels_ = getChannelCount(Vst::kEvent, Vst::kInput, Vst::kMain);
    numMidiOutChannels_ = getChannelCount(Vst::kEvent, Vst::kOutput, Vst::kMain);
    numInputBusses_ = std::min<int>(2, component_->getBusCount(Vst::kAudio, Vst::kInput));
    numInputChannels_[Main] = getNumInputs();
    numInputChannels_[Aux] = getNumAuxInputs();
    numOutputBusses_ = std::min<int>(2, component_->getBusCount(Vst::kAudio, Vst::kOutput));
    numOutputChannels_[Main] = getNumOutputs();
    numOutputChannels_[Aux] = getNumAuxOutputs();
#if 0
    LOG_DEBUG("input busses: " << numInputBusses_ << ", output busses: " << numOutputBusses_);
    LOG_DEBUG("in: " << getNumInputs() << ", auxin: " << getNumAuxInputs()
              << ", out: " << getNumOutputs() << ", auxout: " << getNumAuxOutputs());
#endif
    // setup parameter queues/cache
    // we have to allocate space for all parameters, including hidden ones, e.g. for MIDI CC messages.
    int numParams = controller_->getParameterCount();
    inputParamChanges_.setMaxNumParameters(numParams);
    outputParamChanges_.setMaxNumParameters(numParams);
    paramCache_.resize(numParams);
    updateParamCache();
    LOG_DEBUG("program change: " << info_->programChange);
    LOG_DEBUG("bypass: " << info_->bypass);
}

VST3Plugin::~VST3Plugin(){
    window_ = nullptr;
    processor_ = nullptr;
    // destroy controller
    controller_->terminate();
    controller_ = nullptr;
    LOG_DEBUG("destroyed VST3 controller");
    // destroy component
    component_->terminate();
    component_ = nullptr;
    LOG_DEBUG("destroyed VST3 component");
}

// IComponentHandler
tresult VST3Plugin::beginEdit(Vst::ParamID id){
    LOG_DEBUG("begin edit");
    return kResultOk;
}

tresult VST3Plugin::performEdit(Vst::ParamID id, Vst::ParamValue value){
    int index = info().getParamIndex(id);
    if (index >= 0){
        auto listener = listener_.lock();
        if (listener){
            listener->parameterAutomated(index, value);
        }
        paramCache_[index].value = value;
    }
    if (window_){
        paramChangesFromGui_.push(ParamChange(id, value));
    }
    return kResultOk;
}

tresult VST3Plugin::endEdit(Vst::ParamID id){
    LOG_DEBUG("end edit");
    return kResultOk;
}

tresult VST3Plugin::restartComponent(int32 flags){
#define PRINT_FLAG(name) if (flags & name) { LOG_DEBUG(#name); }
    PRINT_FLAG(Vst::kReloadComponent)
    PRINT_FLAG(Vst::kIoChanged)
    PRINT_FLAG(Vst::kParamValuesChanged)
    PRINT_FLAG(Vst::kLatencyChanged)
    PRINT_FLAG(Vst::kParamTitlesChanged)
    PRINT_FLAG(Vst::kMidiCCAssignmentChanged)
    PRINT_FLAG(Vst::kNoteExpressionChanged)
    PRINT_FLAG(Vst::kIoTitlesChanged)
    PRINT_FLAG(Vst::kPrefetchableSupportChanged)
    PRINT_FLAG(Vst::kRoutingInfoChanged)
#undef PRINT_FLAG

    if (flags & Vst::kLatencyChanged){
        auto listener = listener_.lock();
        if (listener){
            listener->latencyChanged(processor_->getLatencySamples());
        }
    }

    return kResultOk;
}

tresult VST3Plugin::connect(Vst::IConnectionPoint *other){
    LOG_DEBUG("connected!");
    return kResultTrue;
}

tresult VST3Plugin::disconnect(Vst::IConnectionPoint *other){
    LOG_DEBUG("disconnected!");
    return kResultTrue;
}

void printMessage(Vst::IMessage *message){
    LOG_DEBUG("got message: " << message->getMessageID());
    auto msg = dynamic_cast<HostMessage *>(message);
    if (msg){
        msg->print();
    }
}

tresult VST3Plugin::notify(Vst::IMessage *message){
#if LOGLEVEL > 2
    printMessage(message);
#endif
    sendMessage(message);
    return kResultTrue;
}

void VST3Plugin::setupProcessing(double sampleRate, int maxBlockSize, ProcessPrecision precision){
    Vst::ProcessSetup setup;
    setup.processMode = Vst::kRealtime;
    setup.maxSamplesPerBlock = maxBlockSize;
    setup.sampleRate = sampleRate;
    setup.symbolicSampleSize = (precision == ProcessPrecision::Double) ? Vst::kSample64 : Vst::kSample32;
    processor_->setupProcessing(setup);

    context_.sampleRate = sampleRate;
    // update project time in samples (assumes the tempo is valid for the whole project)
    double time = context_.projectTimeMusic / context_.tempo * 60.f;
    context_.projectTimeSamples = time * sampleRate;
    context_.continousTimeSamples = time * sampleRate;
}

void VST3Plugin::process(ProcessData<float>& data){
    doProcess(data);
}

void VST3Plugin::process(ProcessData<double>& data){
    doProcess(data);
}

void setAudioBusBuffers(Vst::AudioBusBuffers& bus, float ** vec){
    bus.channelBuffers32 = vec;
}

void setAudioBusBuffers(Vst::AudioBusBuffers& bus, double ** vec){
    bus.channelBuffers64 = vec;
}

template<typename T>
void doBypassProcessing(IPlugin::ProcessData<T>& data, T** output, int numOut, T** auxoutput, int numAuxOut){
    for (int i = 0; i < numOut; ++i){
        auto out = output[i];
        if (i < data.numInputs){
            // copy input to output
            auto in = data.input[i];
            std::copy(in, in + data.numSamples, out);
        } else {
            std::fill(out, out + data.numSamples, 0); // zero
        }
    }
    for (int i = 0; i < numAuxOut; ++i){
        auto out = auxoutput[i];
        if (i < data.numAuxInputs){
            // copy input to output
            auto in = data.auxInput[i];
            std::copy(in, in + data.numSamples, out);
        } else {
            std::fill(out, out + data.numSamples, 0); // zero
        }
    }
}

template<typename T>
void VST3Plugin::doProcess(ProcessData<T>& inData){
    // process data
    Vst::ProcessData data;
    Vst::AudioBusBuffers input[2];
    Vst::AudioBusBuffers output[2];
    data.numSamples = inData.numSamples;
    data.symbolicSampleSize = std::is_same<T, double>::value ? Vst::kSample64 : Vst::kSample32;
    // we send data for all busses (max. 2 per direction); some might have been deactivated in 'setNumSpeakers'
    data.numInputs = numInputBusses_;
    data.numOutputs = numOutputBusses_;
    data.inputs = input;
    data.outputs = output;
    data.processContext = &context_;
    data.inputEvents = &inputEvents_;
    data.outputEvents = &outputEvents_;
    data.inputParameterChanges = &inputParamChanges_;
    data.outputParameterChanges = &outputParamChanges_;

    auto bypassState = bypass_; // do we have to care about bypass?
    bool bypassRamp = (bypass_ != lastBypass_);
    int rampDir = 0;
    T rampAdvance = 0;
    if (bypassRamp){
        if ((bypass_ == Bypass::Hard || lastBypass_ == Bypass::Hard)){
            // hard bypass: just crossfade to unprocessed input - but keep processing
            // till the *plugin output* is silent (this will clear delay lines, for example).
            bypassState = Bypass::Hard;
        } else if (bypass_ == Bypass::Soft || lastBypass_ == Bypass::Soft){
            // soft bypass: we pass an empty input to the plugin until the output is silent
            // and mix it with the original input. This means that a reverb tail will decay
            // instead of being cut off!
            bypassState = Bypass::Soft;
        }
        rampDir = bypass_ != Bypass::Off;
        rampAdvance = (1.f / data.numSamples) * (1 - 2 * rampDir);
    }
    if ((bypassState == Bypass::Hard) && hasBypass()){
        // if we request a hard bypass from a plugin which has its own bypass method,
        // we use that instead (by just calling the processing method)
        bypassState = Bypass::Off;
        bypassRamp = false;
    }
    lastBypass_ = bypass_;

    // create dummy buffers
    auto indummy = (T *)alloca(sizeof(T) * data.numSamples); // dummy input buffer
    std::fill(indummy, indummy + data.numSamples, 0); // zero!
    auto outdummy = (T *)alloca(sizeof(T) * data.numSamples); // (don't have to zero)

    // prepare input buffers
    auto setInputBuffers = [&](Vst::AudioBusBuffers& bus, auto **vec, int numChannels,
                            auto** inputs, int numIn, auto** outputs, int numOut, auto* dummy){
        bus.silenceFlags = 0;
        bus.numChannels = numChannels;
        for (int i = 0; i < numChannels; ++i){
            if (i < numIn){
                switch (bypassState){
                case Bypass::Soft:
                    // fade input to produce a smooth tail with no click
                    if (bypassRamp && i < numOut){
                        // write fade in/fade out to *output buffer* and use it as the plugin input.
                        // this works because VST plugins actually work in "replacing" mode.
                        auto src = inputs[i];
                        auto dst = vec[i] = outputs[i];
                        T mix = rampDir;
                        for (int j = 0; j < inData.numSamples; ++j, mix += rampAdvance){
                            dst[j] = src[j] * mix;
                        }
                    } else {
                        vec[i] = dummy; // silence
                    }
                    break;
                case Bypass::Hard:
                    if (bypassRamp){
                        // point to input (for creating the ramp)
                        vec[i] = inputs[i];
                    } else {
                        vec[i] = dummy; // silence (for flushing the effect)
                    }
                    break;
                default:
                    // point to input
                    vec[i] = inputs[i];
                    break;
                }
            } else {
                vec[i] = dummy; // silence
            }
        }
        setAudioBusBuffers(bus, vec);
    };
    // prepare output buffers
    auto setOutputBuffers = [](Vst::AudioBusBuffers& bus, auto **vec, int numChannels,
                            auto** outputs, int numOut, auto* dummy){
        bus.silenceFlags = 0;
        bus.numChannels = numChannels;
        for (int i = 0; i < numChannels; ++i){
            if (i < numOut){
                vec[i] = outputs[i];
            } else {
                vec[i] = dummy; // point to dummy buffer
            }
        }
        setAudioBusBuffers(bus, vec);
    };
    // input:
    auto nin = numInputChannels_[Main];
    auto invec = (T **)alloca(sizeof(T*) * nin);
    setInputBuffers(input[Main], invec, nin, (T **)inData.input, inData.numInputs,
            inData.output, inData.numOutputs, indummy);
    // aux input:
    auto nauxin = numInputChannels_[Aux];
    auto auxinvec = (T **)alloca(sizeof(T*) * nauxin);
    setInputBuffers(input[Aux], auxinvec, nauxin, (T **)inData.auxInput, inData.numAuxInputs,
            inData.auxOutput, inData.numAuxOutputs, indummy);
    // output:
    auto nout = numOutputChannels_[Main];
    auto outvec = (T **)alloca(sizeof(T*) * nout);
    setOutputBuffers(output[Main], outvec, nout, (T **)inData.output, inData.numOutputs, outdummy);
    // aux output:
    auto nauxout = numOutputChannels_[Aux];
    auto auxoutvec = (T **)alloca(sizeof(T*) * nauxout);
    setOutputBuffers(output[Aux], auxoutvec, nauxout, (T **)inData.auxOutput, inData.numAuxOutputs, outdummy);

    // send parameter changes from editor to processor
    ParamChange paramChange;
    while (paramChangesFromGui_.pop(paramChange)){
        int32 index;
        auto queue = inputParamChanges_.addParameterData(paramChange.id, index);
        queue->addPoint(0, paramChange.value, index);
    }

    // process
    if (bypassState == Bypass::Off){
        // ordinary processing
        processor_->process(data);
    } else if (bypassRamp) {
        // process <-> bypass transition
        processor_->process(data);

        if (bypassState == Bypass::Soft){
            // soft bypass
            auto softRamp = [&](auto inputs, auto numIn, auto outputs, auto numOut){
                for (int i = 0; i < numOut; ++i){
                    T mix = rampDir;
                    auto out = outputs[i];
                    if (i < numIn){
                        // fade in/out unprocessed input
                        auto in = inputs[i];
                        for (int j = 0; j < data.numSamples; ++j, mix += rampAdvance){
                            out[j] += in[j] * (1.f - mix);
                        }
                    } else {
                        // just fade in/out
                        for (int j = 0; j < data.numSamples; ++j, mix += rampAdvance){
                            out[j] *= mix;
                        }
                    }
                }
            };
            softRamp(inData.input, inData.numInputs, outvec, nout);
            softRamp(inData.auxInput, inData.numAuxInputs, auxoutvec, nauxout);
            if (rampDir){
                LOG_DEBUG("process -> soft bypass");
            } else {
                LOG_DEBUG("soft bypass -> process");
            }
        } else {
            // hard bypass
            auto hardRamp = [&](auto inputs, auto numIn, auto outputs, auto numOut){
                for (int i = 0; i < numOut; ++i){
                   T mix = rampDir;
                   auto out = outputs[i];
                   if (i < numIn){
                       // cross fade between plugin output and unprocessed input
                       auto in = inputs[i];
                       for (int j = 0; j < data.numSamples; ++j, mix += rampAdvance){
                           out[j] = out[j] * mix + in[j] * (1.f - mix);
                       }
                   } else {
                       // just fade out
                       for (int j = 0; j < data.numSamples; ++j, mix += rampAdvance){
                           out[j] *= mix;
                       }
                   }
                }
            };
            hardRamp(inData.input, inData.numInputs, outvec, nout);
            hardRamp(inData.auxInput, inData.numAuxInputs, auxoutvec, nauxout);
            if (rampDir){
                LOG_DEBUG("process -> hard bypass");
            } else {
                LOG_DEBUG("hard bypass -> process");
            }
        }
    } else if (!bypassSilent_){
        // continue to process with empty input till the output is silent
        processor_->process(data);
        // check for silence (RMS < ca. -80dB)
        auto isChannelSilent = [](auto buf, auto n){
            const T threshold = 0.0001;
            T sum = 0;
            for (int i = 0; i < n; ++i){
                T f = buf[i];
                sum += f * f;
            }
            return (sum / n) < (threshold * threshold); // sqrt(sum/n) < threshold
        };
        bool silent = true;
        auto checkBusSilent = [&](auto bus, auto count, auto n){
            for (int i = 0; i < count; ++i){
                if (!isChannelSilent(bus[i], n)){
                    silent = false;
                    break;
                }
            }
        };
        checkBusSilent(outvec, nout, data.numSamples);
        checkBusSilent(auxoutvec, nauxout, data.numSamples);
        if (silent){
            LOG_DEBUG("plugin output became silent!");
        }
        bypassSilent_ = silent;
        if (bypassState == Bypass::Soft){
            auto mix = [](auto** inputs, auto numIn, auto** outputs, auto numOut, auto n){
                // mix output with unprocessed input
                for (int i = 0; i < numOut; ++i){
                    auto out = outputs[i];
                    if (i < numIn){
                        auto in = inputs[i];
                        for (int j = 0; j < n; ++j){
                            out[j] += in[j];
                        }
                    }
                }
            };
            mix(inData.input, inData.numInputs, outvec, nout, inData.numSamples);
            mix(inData.auxInput, inData.numAuxInputs, auxoutvec, nauxout, inData.numSamples);
        } else {
            // overwrite output
            doBypassProcessing(inData, outvec, nout, auxoutvec, nauxout);
        }
    } else {
        // simple bypass
        doBypassProcessing(inData, outvec, getNumOutputs(), auxoutvec, getNumAuxOutputs());
    }
    // clear remaining outputs
    auto clearOutputs = [](auto _outputs, int numOutputs, int numChannels, int n){
        int rem = numOutputs - numChannels;
        for (int i = 0; i < rem; ++i){
            auto out = _outputs[numChannels + i];
            std::fill(out, out + n, 0);
        }
    };
    clearOutputs(inData.output, inData.numOutputs, nout, inData.numSamples);
    clearOutputs(inData.auxOutput, inData.numAuxOutputs, nauxout, inData.numSamples);

    // clear input queues
    inputEvents_.clear();
    inputParamChanges_.clear();

    // handle outgoing events
    handleEvents();
    handleOutputParameterChanges();

    // update time info (if playing)
    if (context_.state & Vst::ProcessContext::kPlaying){
        context_.continousTimeSamples += data.numSamples;
        context_.projectTimeSamples += data.numSamples;
        double projectTimeSeconds = (double)context_.projectTimeSamples / context_.sampleRate;
        // advance project time in bars
        double delta = data.numSamples / context_.sampleRate;
        double beats = delta * context_.tempo / 60.0;
        context_.projectTimeMusic += beats;
        // bar start: simply round project time (in bars) down to bar length
        double barLength = context_.timeSigNumerator * context_.timeSigDenominator / 4.0;
        context_.barPositionMusic = static_cast<int64_t>(context_.projectTimeMusic / barLength) * barLength;
        // SMPTE offset
        double smpteFrames = projectTimeSeconds / context_.frameRate.framesPerSecond;
        double smpteFrameFract = smpteFrames - static_cast<int64_t>(smpteFrames);
        context_.smpteOffsetSubframes = smpteFrameFract * 80; // subframes are 1/80 of a frame
        // MIDI clock offset
        double clockTicks = context_.projectTimeMusic * 24.0;
        double clockTickFract = clockTicks - static_cast<int64_t>(clockTicks);
        // get offset to nearest tick -> can be negative!
        if (clockTickFract > 0.5){
            clockTickFract -= 1.0;
        }
        if (context_.tempo > 0){
            double samplesPerClock = (2.5 / context_.tempo) * context_.sampleRate; // 60.0 / 24.0 = 2.5
            context_.samplesToNextClock = clockTickFract * samplesPerClock;
        } else {
            context_.samplesToNextClock = 0;
        }
    }
}

#define norm2midi(x) (static_cast<uint8_t>((x) * 127.f) & 127)

void VST3Plugin::handleEvents(){
    auto listener = listener_.lock();
    if (listener){
        for (int i = 0; i < outputEvents_.getEventCount(); ++i){
            Vst::Event event;
            outputEvents_.getEvent(i, event);
            if (event.type != Vst::Event::kDataEvent){
                MidiEvent e;
                switch (event.type){
                case Vst::Event::kNoteOffEvent:
                    e.data[0] = 0x80 | event.noteOff.channel;
                    e.data[1] = event.noteOff.pitch;
                    e.data[2] = norm2midi(event.noteOff.velocity);
                    break;
                case Vst::Event::kNoteOnEvent:
                    e.data[0] = 0x90 | event.noteOn.channel;
                    e.data[1] = event.noteOn.pitch;
                    e.data[2] = norm2midi(event.noteOn.velocity);
                    break;
                case Vst::Event::kPolyPressureEvent:
                    e.data[0] = 0xa0 | event.polyPressure.channel;
                    e.data[1] = event.polyPressure.pitch;
                    e.data[2] = norm2midi(event.polyPressure.pressure);
                    break;
                case Vst::Event::kLegacyMIDICCOutEvent:
                    switch (event.midiCCOut.controlNumber){
                    case Vst::kCtrlPolyPressure:
                        e.data[0] = 0x0a | event.midiCCOut.channel;
                        e.data[1] = event.midiCCOut.value;
                        e.data[2] = event.midiCCOut.value2;
                        break;
                    case Vst::kCtrlProgramChange:
                        e.data[0] = 0x0c | event.midiCCOut.channel;
                        e.data[1] = event.midiCCOut.value;
                        e.data[2] = event.midiCCOut.value2;
                        break;
                    case Vst::kAfterTouch:
                        e.data[0] = 0x0d | event.midiCCOut.channel;
                        e.data[1] = event.midiCCOut.value;
                        e.data[2] = event.midiCCOut.value2;
                        break;
                    case Vst::kPitchBend:
                        e.data[0] = 0x0e | event.midiCCOut.channel;
                        e.data[1] = event.midiCCOut.value;
                        e.data[2] = event.midiCCOut.value2;
                        break;
                    default: // CC
                        e.data[0] = 0xb0 | event.midiCCOut.channel;
                        e.data[1] = event.midiCCOut.controlNumber;
                        e.data[2] = event.midiCCOut.value;
                    }
                    break;
                default:
                    LOG_DEBUG("got unsupported event type: " << event.type);
                    continue;
                }
                listener->midiEvent(e);
            } else {
                if (event.data.type == Vst::DataEvent::kMidiSysEx){
                    SysexEvent e((const char *)event.data.bytes, event.data.size);
                    listener->sysexEvent(e);
                } else {
                    LOG_DEBUG("got unsupported data event");
                }
            }
        }
        outputEvents_.clear();
    }
}

void VST3Plugin::handleOutputParameterChanges(){
    auto listener = listener_.lock();
    if (listener){
        int numParams = outputParamChanges_.getParameterCount();
        for (int i = 0; i < numParams; ++i){
            auto data = outputParamChanges_.getParameterData(i);
            if (data){
                auto id = data->getParameterId();
                int numPoints = data->getPointCount();
                auto index = info().getParamIndex(id);
                if (index >= 0){
                    // automatable parameter
                    for (int j = 0; j < numPoints; ++j){
                        int32 offset = 0;
                        Vst::ParamValue value = 0;
                        if (data->getPoint(j, offset, value) == kResultOk){
                            // for now we ignore the sample offset
                            listener->parameterAutomated(index, value);
                        }
                    }
                } else if (window_){
                    // non-automatable parameter (e.g. VU meter)
                    for (int j = 0; j < numPoints; ++j){
                        int32 offset = 0;
                        Vst::ParamValue value = 0;
                        if (data->getPoint(j, offset, value) == kResultOk){
                            paramChangesToGui_.emplace(id, value);
                        }
                    }
                }
            }
        }
    }
    outputParamChanges_.clear();
}

bool VST3Plugin::hasPrecision(ProcessPrecision precision) const {
    switch (precision){
    case ProcessPrecision::Single:
        return processor_->canProcessSampleSize(Vst::kSample32) == kResultTrue;
    case ProcessPrecision::Double:
        return processor_->canProcessSampleSize(Vst::kSample64) == kResultTrue;
    default:
        return false;
    }
}

void VST3Plugin::suspend(){
    processor_->setProcessing(false);
    component_->setActive(false);
}

void VST3Plugin::resume(){
    component_->setActive(true);
    processor_->setProcessing(true);
}

int VST3Plugin::getNumInputs() const {
    return info_->numInputs;
}

int VST3Plugin::getNumAuxInputs() const {
    return info_->numAuxInputs;
}

int VST3Plugin::getNumOutputs() const {
    return info_->numOutputs;
}

int VST3Plugin::getNumAuxOutputs() const {
    return info_->numAuxOutputs;
}

bool VST3Plugin::isSynth() const {
    if (info_){
        return info_->isSynth();
    } else {
        return false;
    }
}

bool VST3Plugin::hasTail() const {
    return getTailSize() != 0;
}

int VST3Plugin::getTailSize() const {
    return processor_->getTailSamples();
}

bool VST3Plugin::hasBypass() const {
    return info().bypass != PluginInfo::NoParamID;
}

void VST3Plugin::setBypass(Bypass state){
    auto bypassID = info().bypass;
    bool haveBypass = bypassID != PluginInfo::NoParamID;
    if (state != bypass_){
        if (state == Bypass::Off){
            if (haveBypass && (bypass_ == Bypass::Hard)){
                doSetParameter(bypassID, 0);
            }
            // soft bypass is handled by us
        } else if (bypass_ == Bypass::Off){
            if (haveBypass && (state == Bypass::Hard)){
                doSetParameter(bypassID, 1);
            }
            // soft bypass is handled by us
        } else {
            // ignore attempts at Bypass::Hard <-> Bypass::Soft!
            return;
        }
        lastBypass_ = bypass_;
        bypass_ = state;
        bypassSilent_ = false;
    }
}

static uint64_t makeChannels(int n){
    return ((uint64_t)1 << n - 1);
}

void VST3Plugin::setNumSpeakers(int in, int out, int auxIn, int auxOut){
    // NOTE: for convenience, we assume that the first bus is 'main' and the second bus is 'aux',
    // but this is not necessarily true...
    LOG_DEBUG("requested bus arrangement: in " << in << ", auxin " << auxIn << ", out " << out << ", auxout " << auxOut);

    // input busses (max. 2)
    Vst::SpeakerArrangement inputBusses[2] = {
        makeChannels(std::min<int>(in, getNumInputs())),
        makeChannels(std::min<int>(auxIn, getNumAuxInputs()))
    };
    // output busses (max. 2)
    Vst::SpeakerArrangement outputBusses[2] = {
        makeChannels(std::min<int>(out, getNumOutputs())),
        makeChannels(std::min<int>(auxOut, getNumAuxOutputs()))
    };
    processor_->setBusArrangements(inputBusses, numInputBusses_, outputBusses, numOutputBusses_);
    // we have to (de)activate busses *after* setting the bus arrangement.
    // we also retrieve and save the actual bus channel counts.
    for (int i = 0; i < numInputBusses_; ++i){
        bool active = i > 0 ? auxIn : in;
        component_->activateBus(Vst::kAudio, Vst::kInput, i, active);
        if (processor_->getBusArrangement(Vst::kInput, i, inputBusses[i]) == kResultOk){
            numInputChannels_[i] = Vst::SpeakerArr::getChannelCount(inputBusses[i]);
        } else {
            numInputChannels_[i] = i > 0 ? getNumAuxInputs() : getNumInputs();
            LOG_WARNING("setNumSpeakers: getBusArrangement not supported");
        }
    }
    for (int i = 0; i < numOutputBusses_; ++i){
        bool active = i > 0 ? auxOut : out;
        component_->activateBus(Vst::kAudio, Vst::kOutput, i, active);
        if (processor_->getBusArrangement(Vst::kOutput, i, outputBusses[i]) == kResultOk){
            numOutputChannels_[i] = Vst::SpeakerArr::getChannelCount(outputBusses[i]);
        } else {
            numOutputChannels_[i] = i > 0 ? getNumAuxOutputs() : getNumOutputs();
            LOG_WARNING("setNumSpeakers: getBusArrangement not supported");
        }
    }
    LOG_DEBUG("actual bus arrangement: in " << numInputChannels_[Main]
              << ", auxin " << numInputChannels_[Aux]
              << ", out " << numOutputChannels_[Main]
              << ", auxout " << numOutputChannels_[Aux]);
}

int VST3Plugin::getLatencySamples() {
    return processor_->getLatencySamples();
}

void VST3Plugin::setTempoBPM(double tempo){
    if (tempo > 0){
        context_.tempo = tempo;
    } else {
        LOG_ERROR("tempo must be greater than 0!");
    }
}

void VST3Plugin::setTimeSignature(int numerator, int denominator){
    context_.timeSigNumerator = numerator;
    context_.timeSigDenominator = denominator;
}

void VST3Plugin::setTransportPlaying(bool play){
    if (play){
        context_.state |= Vst::ProcessContext::kPlaying; // set flag
    } else {
        context_.state &= ~Vst::ProcessContext::kPlaying; // clear flag
    }
}

void VST3Plugin::setTransportRecording(bool record){
    if (record){
        context_.state |= Vst::ProcessContext::kRecording; // set flag
    } else {
        context_.state &= ~Vst::ProcessContext::kRecording; // clear flag
    }
}

void VST3Plugin::setTransportAutomationWriting(bool writing){
    if (writing){
        automationState_ |= Vst::IAutomationState::kWriteState;
    } else {
        automationState_ &= ~Vst::IAutomationState::kWriteState;
    }
    updateAutomationState();
}

void VST3Plugin::setTransportAutomationReading(bool reading){
    if (reading){
        automationState_ |= Vst::IAutomationState::kReadState;
    } else {
        automationState_ &= ~Vst::IAutomationState::kReadState;
    }
    updateAutomationState();
}

void VST3Plugin::updateAutomationState(){
    if (window_){
        automationStateChanged_ = true;
    } else {
        FUnknownPtr<Vst::IAutomationState> automationState(controller_);
        if (automationState){
            automationState->setAutomationState(automationState_);
        }
    }
}

void VST3Plugin::setTransportCycleActive(bool active){
    if (active){
        context_.state |= Vst::ProcessContext::kCycleActive; // set flag
    } else {
        context_.state &= ~Vst::ProcessContext::kCycleActive; // clear flag
    }
}

void VST3Plugin::setTransportCycleStart(double beat){
    context_.cycleStartMusic = beat;
}

void VST3Plugin::setTransportCycleEnd(double beat){
    context_.cycleEndMusic = beat;
}

void VST3Plugin::setTransportPosition(double beat){
    context_.projectTimeMusic = beat;
    // update project time in samples (assuming the tempo is valid for the whole "project")
    double time = beat / context_.tempo * 60.0;
    context_.projectTimeSamples = time * context_.sampleRate;
}

double VST3Plugin::getTransportPosition() const {
    return context_.projectTimeMusic;
}

int VST3Plugin::getNumMidiInputChannels() const {
    return numMidiInChannels_;
}

int VST3Plugin::getNumMidiOutputChannels() const {
    return numMidiOutChannels_;
}

bool VST3Plugin::hasMidiInput() const {
    return numMidiInChannels_ != 0;
}

bool VST3Plugin::hasMidiOutput() const {
    return numMidiOutChannels_ != 0;
}

void VST3Plugin::sendMidiEvent(const MidiEvent &event){
    Vst::Event e;
    e.busIndex = 0;
    e.sampleOffset = event.delta;
    e.ppqPosition = context_.projectTimeMusic;
    e.flags = Vst::Event::kIsLive;
    auto status = event.data[0] & 0xf0;
    auto channel = event.data[0] & 0x0f;
    auto data1 = event.data[1] & 127;
    auto data2 = event.data[2] & 127;
    switch (status){
    case 0x80: // note off
        e.type = Vst::Event::kNoteOffEvent;
        e.noteOff.channel = channel;
        e.noteOff.noteId = -1;
        e.noteOff.pitch = data1;
        e.noteOff.velocity = data2 / 127.f;
        e.noteOff.tuning = event.detune;
        break;
    case 0x90: // note on
        e.type = Vst::Event::kNoteOnEvent;
        e.noteOn.channel = channel;
        e.noteOn.noteId = -1;
        e.noteOn.pitch = data1;
        e.noteOn.velocity = data2 / 127.f;
        e.noteOn.length = 0;
        e.noteOn.tuning = event.detune;
        break;
    case 0xa0: // polytouch
        e.type = Vst::Event::kPolyPressureEvent;
        e.polyPressure.channel = channel;
        e.polyPressure.pitch = data1;
        e.polyPressure.pressure = data2 / 127.f;
        e.polyPressure.noteId = -1;
        break;
    case 0xb0: // CC
        {
            Vst::ParamID id = Vst::kNoParamId;
            FUnknownPtr<Vst::IMidiMapping> midiMapping(controller_);
            if (midiMapping && midiMapping->getMidiControllerAssignment (0, channel, data1, id) == kResultOk){
                doSetParameter(id, data2 / 127.f, event.delta);
            } else {
                LOG_WARNING("MIDI CC control number " << data1 << " not supported");
            }
            return;
        }
    case 0xc0: // program change
        {
            auto id = info().programChange;
            if (id != PluginInfo::NoParamID){
                doSetParameter(id, data1 / 127.f);
            #if 0
                program_ = data1;
            #endif
                updateParamCache();
            } else {
                LOG_DEBUG("no program change parameter");
            }
            return;
        }
    case 0xd0: // channel aftertouch
        {
            Vst::ParamID id = Vst::kNoParamId;
            FUnknownPtr<Vst::IMidiMapping> midiMapping(controller_);
            if (midiMapping && midiMapping->getMidiControllerAssignment (0, channel, Vst::kAfterTouch, id) == kResultOk){
                doSetParameter(id, data1 / 127.f, event.delta);
            } else {
                LOG_WARNING("MIDI channel aftertouch not supported");
            }
            return;
        }
    case 0xe0: // pitch bend
        {
            Vst::ParamID id = Vst::kNoParamId;
            FUnknownPtr<Vst::IMidiMapping> midiMapping(controller_);
            if (midiMapping && midiMapping->getMidiControllerAssignment (0, channel, Vst::kPitchBend, id) == kResultOk){
                uint32_t bend = data1 | (data2 << 7);
                doSetParameter(id, (float)bend / 16383.f, event.delta);
            } else {
                LOG_WARNING("MIDI pitch bend not supported");
            }
            return;
        }
    default:
        LOG_WARNING("MIDI system messages not supported!");
        return;
    }
    inputEvents_.addEvent(e);
}

void VST3Plugin::sendSysexEvent(const SysexEvent &event){
    inputEvents_.addSysexEvent(event);
}

void VST3Plugin::setParameter(int index, float value, int sampleOffset){
    auto id = info().getParamID(index);
    doSetParameter(id, value, sampleOffset);
}

bool VST3Plugin::setParameter(int index, const std::string &str, int sampleOffset){
    Vst::ParamValue value;
    Vst::String128 string;
    auto id = info().getParamID(index);
    if (convertString(str, string)){
        if (controller_->getParamValueByString(id, string, value) == kResultOk){
            doSetParameter(id, value, sampleOffset);
            return true;
        }
    }
    return false;
}

void VST3Plugin::doSetParameter(Vst::ParamID id, float value, int32 sampleOffset){
    int32 dummy;
    inputParamChanges_.addParameterData(id, dummy)->addPoint(sampleOffset, value, dummy);
    auto index = info().getParamIndex(id);
    if (index >= 0){
        // automatable parameter
    #if 1
        // verify parameter value
        value = controller_->normalizedParamToPlain(id, value);
        value = controller_->plainParamToNormalized(id, value);
    #endif
        paramCache_[index].value = value;
        if (window_){
            paramCache_[index].changed = true;
        } else {
            controller_->setParamNormalized(id, value);
        }
    } else {
        // non-automatable parameter
        if (window_){
            paramChangesToGui_.emplace(id, value);
        } else {
            controller_->setParamNormalized(id, value);
        }
    }
}

float VST3Plugin::getParameter(int index) const {
    return paramCache_[index].value;
}

std::string VST3Plugin::getParameterString(int index) const {
    Vst::String128 display;
    Vst::ParamID id = info().getParamID(index);
    float value = paramCache_[index].value;
    if (controller_->getParamStringByValue(id, value, display) == kResultOk){
        return convertString(display);
    }
    return std::string{};
}

int VST3Plugin::getNumParameters() const {
    return info().numParameters();
}

void VST3Plugin::updateParamCache(){
    for (int i = 0; i < (int)paramCache_.size(); ++i){
        auto id = info().getParamID(i);
        paramCache_[i].value = controller_->getParamNormalized(id);
        // we don't need to tell the GUI to update
        // paramCache_[i].changed = true;
    }
}

void VST3Plugin::setProgram(int program){
    if (program >= 0 && program < getNumPrograms()){
        auto id = info().programChange;
        if (id != PluginInfo::NoParamID){
            auto value = controller_->plainParamToNormalized(id, program);
            doSetParameter(id, value);
            program_ = program;
            updateParamCache();
        } else {
            LOG_DEBUG("no program change parameter");
        }
    } else {
        LOG_WARNING("program number out of range!");
    }
}

void VST3Plugin::setProgramName(const std::string& name){
    // ?
}

int VST3Plugin::getProgram() const {
    return program_;
}

std::string VST3Plugin::getProgramName() const {
    return getProgramNameIndexed(getProgram());
}

std::string VST3Plugin::getProgramNameIndexed(int index) const {
    if (index >= 0 && index < info().numPrograms()){
        return info().programs[index];
    } else {
        return "";
    }
}

int VST3Plugin::getNumPrograms() const {
    return info().numPrograms();
}

void VST3Plugin::readProgramFile(const std::string& path){
    std::ifstream file(path, std::ios_base::binary);
    if (!file.is_open()){
        throw Error("couldn't open file " + path);
    }
    std::string buffer;
    file.seekg(0, std::ios_base::end);
    buffer.resize(file.tellg());
    file.seekg(0, std::ios_base::beg);
    file.read(&buffer[0], buffer.size());
    readProgramData(buffer.data(), buffer.size());
}

struct ChunkListEntry {
    Vst::ChunkID id = { 0 };
    int64 offset = 0;
    int64 size = 0;
};

void VST3Plugin::readProgramData(const char *data, size_t size){
    ConstStream stream(data, size);
    std::vector<ChunkListEntry> entries;
    auto isChunkType = [](Vst::ChunkID id, Vst::ChunkType type){
        return memcmp(id, Vst::getChunkID(type), sizeof(Vst::ChunkID)) == 0;
    };
    auto checkChunkID = [&](Vst::ChunkType type){
        Vst::ChunkID id;
        stream.readChunkID(id);
        if (!isChunkType(id, type)){
            throw Error("bad chunk ID");
        }
    };
    // read header
    if (size < Vst::kHeaderSize){
        throw Error("too little data");
    }
    checkChunkID(Vst::kHeader);
    int32 version;
    stream.readInt32(version);
    LOG_DEBUG("version: " << version);
    TUID classID;
    stream.readTUID(classID);
    if (memcmp(classID, info().getUID(), sizeof(TUID)) != 0){
    #if 1
        // HACK to allow loading presets of v0.3.0> which had
        // the wrong class ID. This might go away in a later version.
        // 1) reproduce the wrong serialization
        char buf[33];
        for (int i = 0; i < sizeof(TUID); ++i) {
            // this code gave wrong results because of the missing uint8_t cast!
            snprintf(buf + 2 * i, 3, "%02X", classID[i]);
        }
        buf[32] = 0;
        // 2) deserialize
        TUID wrongID;
        for (int i = 0; i < sizeof(TUID); ++i) {
            uint32_t temp;
            sscanf(buf + 2 * i, "%02X", &temp);
            wrongID[i] = temp;
        }
        // 3) compare again
        if (memcmp(classID, wrongID, sizeof(TUID)) == 0) {
            LOG_WARNING("This preset data has a wrong class ID from v0.3.0 or below.\n"
                "Please save it to fix the problem.");
        } else
    #endif
        {
        #if LOGLEVEL > 2
            fprintf(stdout, "preset: ");
            for (int i = 0; i < 16; ++i) {
                fprintf(stdout, "%02X", (uint8_t)classID[i]);
            }
            fprintf(stdout, "\nplugin: %s\n", info().uniqueID.c_str());
            fflush(stdout);
        #endif
            throw Error("wrong class ID");
        }
    }
    int64 offset;
    stream.readInt64(offset);
    // read chunk list
    stream.setPos(offset);
    checkChunkID(Vst::kChunkList);
    int32 count;
    stream.readInt32(count);
    while (count--){
        ChunkListEntry entry;
        stream.readChunkID(entry.id);
        stream.readInt64(entry.offset);
        stream.readInt64(entry.size);
        entries.push_back(entry);
    }
    // get chunk data
    for (auto& entry : entries){
        stream.setPos(entry.offset);
        if (isChunkType(entry.id, Vst::kComponentState)){
            if (component_->setState(&stream) == kResultOk){
                // also update controller state!
                stream.setPos(entry.offset); // rewind
                controller_->setComponentState(&stream); // TODO: make thread-safe
                updateParamCache();
                LOG_DEBUG("restored component state");
            } else {
                LOG_WARNING("couldn't restore component state");
            }
        } else if (isChunkType(entry.id, Vst::kControllerState)){
            // TODO: make thread-safe
            if (controller_->setState(&stream) == kResultOk){
                LOG_DEBUG("restored controller state");
            } else {
                LOG_WARNING("couldn't restore controller state");
            }
        }
    }
}

void VST3Plugin::writeProgramFile(const std::string& path){
    std::ofstream file(path, std::ios_base::binary | std::ios_base::trunc);
    if (!file.is_open()){
        throw Error("couldn't create file " + path);
    }
    std::string buffer;
    writeProgramData(buffer);
    file.write(buffer.data(), buffer.size());
}

void VST3Plugin::writeProgramData(std::string& buffer){
    std::vector<ChunkListEntry> entries;
    WriteStream stream;
    stream.writeChunkID(Vst::getChunkID(Vst::kHeader)); // header
    stream.writeInt32(Vst::kFormatVersion); // version
    stream.writeTUID(info().getUID()); // class ID
    stream.writeInt64(0); // skip offset
    // write data
    auto writeChunk = [&](auto& component, Vst::ChunkType type){
        ChunkListEntry entry;
        memcpy(entry.id, Vst::getChunkID(type), sizeof(Vst::ChunkID));
        stream.tell(&entry.offset);
        // TODO what do for a GUI editor?
        if (component->getState(&stream) == kResultTrue){
            auto pos = stream.getPos();
            entry.size = pos - entry.offset;
            entries.push_back(entry);
        } else {
            LOG_DEBUG("couldn't get state");
            // throw?
        }
    };
    writeChunk(component_, Vst::kComponentState);
    writeChunk(controller_, Vst::kControllerState);
    // store list offset
    auto listOffset = stream.getPos();
    // write list
    stream.writeChunkID(Vst::getChunkID(Vst::kChunkList));
    stream.writeInt32(entries.size());
    for (auto& entry : entries){
        stream.writeChunkID(entry.id);
        stream.writeInt64(entry.offset);
        stream.writeInt64(entry.size);
    }
    // write list offset
    stream.setPos(Vst::kListOffsetPos);
    stream.writeInt64(listOffset);
    // done
    stream.transfer(buffer);
}

void VST3Plugin::readBankFile(const std::string& path){
    throw Error("not implemented");
}

void VST3Plugin::readBankData(const char *data, size_t size){
    throw Error("not implemented");
}

void VST3Plugin::writeBankFile(const std::string& path){
    throw Error("not implemented");
}

void VST3Plugin::writeBankData(std::string& buffer){
    throw Error("not implemented");
}

bool VST3Plugin::hasEditor() const {
    if (!view_){
        view_ = controller_->createView("editor");
    }
    if (view_){
    #if defined(_WIN32)
        return view_->isPlatformTypeSupported("HWND") == kResultOk;
    #elif defined(__APPLE__)
        return view_->isPlatformTypeSupported("NSView") == kResultOk;
        // TODO: check for iOS ("UIView")
    #else
        return view_->isPlatformTypeSupported("X11EmbedWindowID") == kResultOk;
    #endif
    } else {
        return false;
    }
}

tresult VST3Plugin::resizeView(IPlugView *view, ViewRect *newSize){
    LOG_DEBUG("resizeView");
    if (window_){
        window_->setSize(newSize->getWidth(), newSize->getHeight());
    }
    return view->onSize(newSize);
}

void VST3Plugin::openEditor(void * window){
    if (editor_){
        return;
    }
    if (!view_){
        view_ = controller_->createView("editor");
    }
    if (view_){
        view_->setFrame(this);
        LOG_DEBUG("attach view");
    #if defined(_WIN32)
        auto result = view_->attached(window, "HWND");
    #elif defined(__APPLE__)
        auto result = view_->attached(window, "NSView");
        // TODO: iOS ("UIView")
    #else
        auto result = view_->attached(window, "X11EmbedWindowID");
    #endif
        if (result == kResultOk) {
            LOG_DEBUG("opened VST3 editor");
        }
        else {
            LOG_ERROR("couldn't open VST3 editor");
        }
    }
    editor_ = true;
}

void VST3Plugin::closeEditor(){
    if (!editor_){
        return;
    }
    if (!view_){
        view_ = controller_->createView("editor");
    }
    if (view_){
        if (view_->removed() == kResultOk) {
            LOG_DEBUG("closed VST3 editor");
        }
        else {
            LOG_ERROR("couldn't close VST3 editor");
        }
    }
    editor_ = false;
}

bool VST3Plugin::getEditorRect(int &left, int &top, int &right, int &bottom) const {
    if (!view_){
        view_ = controller_->createView("editor");
    }
    if (view_){
        ViewRect rect;
        if (view_->getSize(&rect) == kResultOk){
            left = rect.left;
            top = rect.top;
            right = rect.right;
            bottom = rect.bottom;
            return true;
        }
    }
    return false;
}

void VST3Plugin::updateEditor(){
    // automatable parameters
    int n = paramCache_.size();
    for (int i = 0; i < n; ++i){
        bool expected = true;
        if (paramCache_[i].changed.compare_exchange_strong(expected, false)){
            auto id = info().getParamID(i);
            float value = paramCache_[i].value;
            LOG_DEBUG("update parameter " << id << ": " << value);
            controller_->setParamNormalized(id, value);
        }
    }
    // non-automatable parameters (e.g. VU meter)
    ParamChange p;
    while (paramChangesToGui_.pop(p)){
        controller_->setParamNormalized(p.id, p.value);
    }
    // automation state
    bool expected = true;
    if (automationStateChanged_.compare_exchange_strong(expected, false)){
        FUnknownPtr<Vst::IAutomationState> automationState(controller_);
        if (automationState){
            LOG_DEBUG("update automation state");
            automationState->setAutomationState(automationState_);
        }
    }
}

void VST3Plugin::checkEditorSize(int &width, int &height) const {
    if (!view_){
        view_ = controller_->createView("editor");
    }
    if (view_){
        ViewRect rect(0, 0, width, height);
        if (view_->checkSizeConstraint(&rect) == kResultOk){
            width = rect.getWidth();
            height = rect.getHeight();
        }
    }
}

void VST3Plugin::resizeEditor(int width, int height) {
    if (!view_){
        view_ = controller_->createView("editor");
    }
    if (view_){
        ViewRect rect;
        if (view_->getSize(&rect) == kResultOk){
            rect.right = rect.left + width;
            rect.bottom = rect.top + height;
            if (view_->onSize(&rect) != kResultOk){
                LOG_ERROR("couldn't resize editor");
            }
        } else {
            LOG_ERROR("couldn't get editor size");
        }
    }
}

bool VST3Plugin::canResize() const {
    if (!view_){
        view_ = controller_->createView("editor");
    }
    return view_ && (view_->canResize() == kResultTrue);
}

// VST3 only
void VST3Plugin::beginMessage() {
    msg_.reset(new HostMessage);
}

void VST3Plugin::addInt(const char* id, int64_t value) {
    if (msg_){
        msg_->getAttributes()->setInt(id, value);
    }
}

void VST3Plugin::addFloat(const char* id, double value) {
    if (msg_){
        msg_->getAttributes()->setFloat(id, value);
    }
}

void VST3Plugin::addString(const char* id, const char *value) {
    addString(id, std::string(value));
}

void VST3Plugin::addString(const char* id, const std::string& value) {
    if (msg_){
        Vst::String128 buf;
        convertString(value, buf);
        msg_->getAttributes()->setString(id, buf);
    }
}

void VST3Plugin::addBinary(const char* id, const char *data, size_t size) {
    if (msg_){
        msg_->getAttributes()->setBinary(id, data, size);
    }
}

void VST3Plugin::endMessage() {
    if (msg_){
        sendMessage(msg_.get());
        msg_ = nullptr;
    }
}

void VST3Plugin::sendMessage(Vst::IMessage *msg){
    FUnknownPtr<Vst::IConnectionPoint> p1(component_);
    if (p1){
        p1->notify(msg);
    }
    FUnknownPtr<Vst::IConnectionPoint> p2(controller_);
    if (p2){
        // do we have to call this on the UI thread if we have a GUI editor?
        p2->notify(msg);
    }
}

/*///////////////////// BaseStream ///////////////////////*/

tresult BaseStream::read  (void* buffer, int32 numBytes, int32* numBytesRead){
    int available = size() - cursor_;
    if (available <= 0){
        cursor_ = size();
    }
    if (numBytes > available){
        numBytes = available;
    }
    if (numBytes > 0){
        memcpy(buffer, data() + cursor_, numBytes);
        cursor_ += numBytes;
    }
    if (numBytesRead){
        *numBytesRead = numBytes;
    }
    // LOG_DEBUG("BaseStream: read " << numBytes << " bytes");
    return kResultOk;
}

tresult BaseStream::write (void* buffer, int32 numBytes, int32* numBytesWritten){
    return kNotImplemented;
}

tresult BaseStream::seek  (int64 pos, int32 mode, int64* result){
    if (pos < 0){
        return kInvalidArgument;
    }
    switch (mode){
    case kIBSeekSet:
        cursor_ = pos;
        break;
    case kIBSeekCur:
        cursor_ += pos;
        break;
    case kIBSeekEnd:
        cursor_ = size() + pos;
        break;
    default:
        return kInvalidArgument;
    }
    // don't have to resize here
    if (result){
        *result = cursor_;
    }
    // LOG_DEBUG("BaseStream: set cursor to " << cursor_);
    return kResultTrue;
}

tresult BaseStream::tell  (int64* pos){
    if (pos){
        *pos = cursor_;
        // LOG_DEBUG("BaseStream: told cursor pos");
        return kResultTrue;
    } else {
        return kInvalidArgument;
    }
}

void BaseStream::setPos(int64 pos){
    if (pos >= 0){
        cursor_ = pos;
    } else {
        cursor_ = 0;
    }
}

int64 BaseStream::getPos() const {
    return cursor_;
}

void BaseStream::rewind(){
    cursor_ = 0;
}

template<typename T>
bool BaseStream::doWrite(const T& t){
    int32 bytesWritten = 0;
    write((void *)&t, sizeof(T), &bytesWritten);
    return bytesWritten == sizeof(T);
}

template<typename T>
bool BaseStream::doRead(T& t){
    int32 bytesRead = 0;
    read(&t, sizeof(T), &bytesRead);
    return bytesRead == sizeof(T);
}

bool BaseStream::writeInt32(int32 i){
#if BYTEORDER == kBigEndian
    SWAP_32 (i)
#endif
    return doWrite(i);
}

bool BaseStream::writeInt64(int64 i){
#if BYTEORDER == kBigEndian
    SWAP_64 (i)
#endif
    return doWrite(i);
}

bool BaseStream::writeChunkID(const Vst::ChunkID id){
    int32 bytesWritten = 0;
    write((void *)id, sizeof(Vst::ChunkID), &bytesWritten);
    return bytesWritten == sizeof(Vst::ChunkID);
}

struct GUIDStruct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

bool BaseStream::writeTUID(const TUID tuid){
    int32 bytesWritten = 0;
    int i = 0;
    char buf[Vst::kClassIDSize+1];
#if COM_COMPATIBLE
    GUIDStruct guid;
    memcpy(&guid, tuid, sizeof(GUIDStruct));
    sprintf(buf, "%08X%04X%04X", guid.data1, guid.data2, guid.data3);
    i += 8;
#endif
    for (; i < (int)sizeof(TUID); ++i){
        // we have to cast to uint8_t!
        sprintf(buf + 2 * i, "%02X", (uint8_t)tuid[i]);
    }
    write(buf, Vst::kClassIDSize, &bytesWritten);
    return bytesWritten == Vst::kClassIDSize;
}

bool BaseStream::readInt32(int32& i){
    if (doRead(i)){
    #if BYTEORDER == kBigEndian
        SWAP_32 (i)
    #endif
        return true;
    } else {
        return false;
    }
}

bool BaseStream::readInt64(int64& i){
    if (doRead(i)){
    #if BYTEORDER == kBigEndian
        SWAP_64 (i)
    #endif
        return true;
    } else {
        return false;
    }
}

bool BaseStream::readChunkID(Vst::ChunkID id){
    int32 bytesRead = 0;
    read((void *)id, sizeof(Vst::ChunkID), &bytesRead);
    return bytesRead == sizeof(Vst::ChunkID);
}

bool BaseStream::readTUID(TUID tuid){
    int32 bytesRead = 0;
    char buf[Vst::kClassIDSize+1];
    read((void *)buf, Vst::kClassIDSize, &bytesRead);
    if (bytesRead == Vst::kClassIDSize){
        buf[Vst::kClassIDSize] = 0;
        int i = 0;
    #if COM_COMPATIBLE
        GUIDStruct guid;
        sscanf(buf, "%08x", &guid.data1);
        sscanf(buf+8, "%04hx", &guid.data2);
        sscanf(buf+12, "%04hx", &guid.data3);
        memcpy(tuid, &guid, sizeof(TUID) / 2);
        i += 16;
    #endif
        for (; i < Vst::kClassIDSize; i += 2){
            uint32_t temp;
            sscanf(buf + i, "%02X", &temp);
            tuid[i / 2] = temp;
        }
        return true;
    } else {
        return false;
    }
}

/*///////////////////// ConstStream //////////////////////////*/

ConstStream::ConstStream(const char *data, size_t size){
    assign(data, size);
}

void ConstStream::assign(const char *data, size_t size){
    data_ = data;
    size_ = size;
    cursor_ = 0;
}

/*///////////////////// WriteStream //////////////////////////*/

WriteStream::WriteStream(const char *data, size_t size){
    buffer_.assign(data, size);
}

tresult WriteStream::write (void* buffer, int32 numBytes, int32* numBytesWritten){
    int wantSize = cursor_ + numBytes;
    if (wantSize > (int64_t)buffer_.size()){
        buffer_.resize(wantSize);
    }
    if (cursor_ >= 0 && numBytes > 0){
        memcpy(&buffer_[cursor_], buffer, numBytes);
        cursor_ += numBytes;
    } else {
        numBytes = 0;
    }
    if (numBytesWritten){
        *numBytesWritten = numBytes;
    }
    // LOG_DEBUG("BaseStream: wrote " << numBytes << " bytes");
    return kResultTrue;
}

void WriteStream::transfer(std::string &dest){
    dest = std::move(buffer_);
    cursor_ = 0;
}

/*///////////////////// PlugInterfaceSupport //////////////////////////*/

PlugInterfaceSupport::PlugInterfaceSupport ()
{
    // add minimum set
    //---VST 3.0.0--------------------------------
    addInterface(Vst::IComponent::iid);
    addInterface(Vst::IAudioProcessor::iid);
    addInterface(Vst::IEditController::iid);
    addInterface(Vst::IConnectionPoint::iid);

    addInterface(Vst::IUnitInfo::iid);
    addInterface(Vst::IUnitData::iid);
    addInterface(Vst::IProgramListData::iid);

    //---VST 3.0.1--------------------------------
    addInterface(Vst::IMidiMapping::iid);

    //---VST 3.1----------------------------------
    // addInterface(Vst::EditController2::iid);

    //---VST 3.0.2--------------------------------
    // addInterface(Vst::IParameterFinder::iid);

    //---VST 3.1----------------------------------
    // addInterface(Vst::IAudioPresentationLatency::iid);

    //---VST 3.5----------------------------------
    // addInterface(Vst::IKeyswitchController::iid);
    // addInterface(Vst::IContextMenuTarget::iid);
    // addInterface(Vst::IEditControllerHostEditing::iid);
    // addInterface(Vst::IXmlRepresentationController::iid);
    // addInterface(Vst::INoteExpressionController::iid);

    //---VST 3.6.5--------------------------------
    // addInterface(Vst::ChannelContext::IInfoListener::iid);
    // addInterface(Vst::IPrefetchableSupport::iid);
    addInterface(Vst::IAutomationState::iid);

    //---VST 3.6.11--------------------------------
    // addInterface(Vst::INoteExpressionPhysicalUIMapping::iid);

    //---VST 3.6.12--------------------------------
    // addInterface(Vst::IMidiLearn::iid);
}

tresult PLUGIN_API PlugInterfaceSupport::isPlugInterfaceSupported (const TUID _iid)
{
    for (auto& uid : supportedInterfaces_){
        if (uid == _iid){
            LOG_DEBUG("interface supported!");
            return kResultTrue;
        }
    }
    LOG_DEBUG("interface not supported!");
    return kResultFalse;
}

void PlugInterfaceSupport::addInterface(const TUID _id){
    supportedInterfaces_.emplace_back(_id);
}

/*///////////////////// HostApplication //////////////////////////*/

Vst::IHostApplication *getHostContext(){
    static auto app = new HostApplication;
    return app;
}

HostApplication::HostApplication()
    : interfaceSupport_(std::make_unique<PlugInterfaceSupport>())
{}

HostApplication::~HostApplication() {}

tresult PLUGIN_API HostApplication::getName (Vst::String128 name){
    LOG_DEBUG("host: getName");
#ifdef PD
    convertString("vstplugin~", name);
#else
    convertString("VSTPlugin", name);
#endif
    return kResultTrue;
}

tresult PLUGIN_API HostApplication::createInstance (TUID cid, TUID _iid, void** obj){
    FUID classID(cid);
    FUID interfaceID(_iid);
    // LOG_DEBUG("host: createInstance");
    if (classID == Vst::IMessage::iid && interfaceID == Vst::IMessage::iid)
    {
        // LOG_DEBUG("create HostMessage");
        *obj = (Vst::IMessage *)new HostMessage;
        return kResultTrue;
    }
    else if (classID == Vst::IAttributeList::iid && interfaceID == Vst::IAttributeList::iid)
    {
        // LOG_DEBUG("create HostAttributeList");
        *obj = (Vst::IAttributeList *)new HostAttributeList;
        return kResultTrue;
    }
    *obj = nullptr;
    return kResultFalse;
}

tresult PLUGIN_API HostApplication::queryInterface (const char* _iid, void** obj){
    // LOG_DEBUG("host: query interface");
    QUERY_INTERFACE (_iid, obj, FUnknown::iid, IHostApplication)
    QUERY_INTERFACE (_iid, obj, IHostApplication::iid, IHostApplication)
    if (interfaceSupport_ && interfaceSupport_->queryInterface (iid, obj) == kResultTrue)
        return kResultOk;

    *obj = nullptr;
    return kResultFalse;
}

/*///////////////////// HostAttribute //////////////////////////*/

HostAttribute::HostAttribute(const Vst::TChar* s) : type(kString){
    size = wcslen((const wchar_t *)s);
    if (size > 0){
        v.s = new Vst::TChar[size + 1]; // extra character
        memcpy(v.s, s, size * sizeof(Vst::TChar));
        v.s[size] = 0; // null terminate!
    }
}

HostAttribute::HostAttribute(const char * data, uint32 n) : size(n), type(kBinary){
    v.b = new char[size];
    memcpy(v.s, data, n);
}

HostAttribute::HostAttribute(HostAttribute&& other){
    if (size > 0){
        delete[] v.b;
    }
    type = other.type;
    size = other.size;
    v = other.v;
    // leave other empty
    other.size = 0;
    other.v.b = nullptr; // works for strings as well
}

HostAttribute::~HostAttribute(){
    if (size > 0){
        if (type == kString){
            delete[] v.s;
        } else if (type == kBinary){
            delete[] v.b;
        }
    }
}

/*///////////////////// HostAttributeList //////////////////////////*/

HostAttribute *HostAttributeList::find(AttrID aid) {
    auto it = list_.find(aid);
    if (it != list_.end()){
        return &it->second;
    } else {
        return nullptr;
    }
}

tresult PLUGIN_API HostAttributeList::setInt (AttrID aid, int64 value){
    list_.emplace(aid, HostAttribute(value));
    return kResultTrue;
}

tresult PLUGIN_API HostAttributeList::getInt (AttrID aid, int64& value){
    auto attr = find(aid);
    if (attr && attr->type == HostAttribute::kInteger){
        value = attr->v.i;
        return kResultTrue;
    }
    return kResultFalse;
}

tresult PLUGIN_API HostAttributeList::setFloat (AttrID aid, double value){
    list_.emplace(aid, HostAttribute(value));
    return kResultTrue;
}

tresult PLUGIN_API HostAttributeList::getFloat (AttrID aid, double& value){
    auto attr = find(aid);
    if (attr && attr->type == HostAttribute::kFloat){
        value = attr->v.f;
        return kResultTrue;
    }
    return kResultFalse;
}

tresult PLUGIN_API HostAttributeList::setString (AttrID aid, const Vst::TChar* string){
    list_.emplace(aid, HostAttribute(string));
    return kResultTrue;
}

tresult PLUGIN_API HostAttributeList::getString (AttrID aid, Vst::TChar* string, uint32 size){
    auto attr = find(aid);
    if (attr && attr->type == HostAttribute::kString){
        size = std::min<uint32>(size-1, attr->size); // make room for the null terminator
        memcpy(string, attr->v.s, size * sizeof(Vst::TChar));
        string[size] = 0; // null terminate!
        return kResultTrue;
    }
    return kResultFalse;
}

tresult PLUGIN_API HostAttributeList::setBinary (AttrID aid, const void* data, uint32 size){
    list_.emplace(aid, HostAttribute((const char*)data, size));
    return kResultTrue;
}

tresult PLUGIN_API HostAttributeList::getBinary (AttrID aid, const void*& data, uint32& size){
    auto attr = find(aid);
    if (attr && attr->type == HostAttribute::kBinary){
        data = attr->v.b;
        size = attr->size;
        return kResultTrue;
    }
    return kResultFalse;
}

void HostAttributeList::print(){
    for (auto& it : list_){
        auto& id = it.first;
        auto& attr = it.second;
        switch (attr.type){
        case HostAttribute::kInteger:
            LOG_VERBOSE(id << ": " << attr.v.i);
            break;
        case HostAttribute::kFloat:
            LOG_VERBOSE(id << ": " << attr.v.f);
            break;
        case HostAttribute::kString:
            LOG_VERBOSE(id << ": " << convertString(attr.v.s));
            break;
        case HostAttribute::kBinary:
            LOG_VERBOSE(id << ": [binary]");
            break;
        default:
            LOG_VERBOSE(id << ": ?");
            break;
        }
    }
}

/*///////////////////// HostMessage //////////////////////////*/

Vst::IAttributeList* PLUGIN_API HostMessage::getAttributes () {
    // LOG_DEBUG("HostMessage::getAttributes");
    if (!attributes_){
        attributes_.reset(new HostAttributeList);
    }
#if LOGLEVEL > 2
    attributes_->print();
#endif
    return attributes_.get();
}

void HostMessage::print(){
    if (attributes_){
        attributes_->print();
    }
}

} // vst
