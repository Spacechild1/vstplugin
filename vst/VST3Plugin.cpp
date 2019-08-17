#include "VST3Plugin.h"

#include <cstring>
#include <algorithm>
#include <set>

DEF_CLASS_IID (FUnknown)
DEF_CLASS_IID (IBStream)
// DEF_CLASS_IID (IPlugFrame)
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


using namespace VST3;

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

//------------------------------------------------------------------------

} // Vst
} // Steinberg


namespace vst {

/*/////////////////////// VST3Factory /////////////////////////*/

VST3Factory::VST3Factory(const std::string& path)
    : path_(path)
{
    std::string modulePath = path;
#ifndef __APPLE__
    if (isDirectory(modulePath)){
        modulePath += "/" + getBundleBinaryPath() + "/" + fileName(path);
    }
#endif
    module_ = IModule::load(modulePath); // throws on failure
    auto factoryProc = module_->getFnPtr<GetFactoryProc>("GetPluginFactory");
    if (!factoryProc){
        throw Error("couldn't find 'GetPluginFactory' function");
    }
    if (!module_->init()){
        throw Error("couldn't init module");
    }
    factory_ = IPtr<IPluginFactory>(factoryProc());
    if (!factory_){
        throw Error("couldn't get VST3 plug-in factory");
    }
    /// LOG_DEBUG("VST3Factory: loaded " << path);
    // map plugin names to indices
    auto numPlugins = factory_->countClasses();
    /// LOG_DEBUG("module contains " << numPlugins << " classes");
    for (int i = 0; i < numPlugins; ++i){
        PClassInfo ci;
        if (factory_->getClassInfo(i, &ci) == kResultTrue){
            /// LOG_DEBUG("\t" << ci.name << ", " << ci.category);
            if (!strcmp(ci.category, kVstAudioEffectClass)){
                pluginList_.push_back(ci.name);
                pluginIndexMap_[ci.name] = i;
            }
        } else {
            throw Error("couldn't get class info!");
        }
    }
}

VST3Factory::~VST3Factory(){
    if (!module_->exit()){
        // don't throw!
        LOG_ERROR("couldn't exit module");
    }
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

int VST3Factory::numPlugins() const {
    return plugins_.size();
}

IFactory::ProbeFuture VST3Factory::probeAsync() {
    if (pluginList_.empty()){
        throw Error("factory doesn't have any plugin(s)");
    }
    plugins_.clear();
    pluginMap_.clear();
    auto self(shared_from_this());
    /// LOG_DEBUG("got probePlugin future");
    if (pluginList_.size() > 1){
        return [this, self=std::move(self)](ProbeCallback callback){
            ProbeList pluginList;
            for (auto& name : pluginList_){
                pluginList.emplace_back(name, 0);
            }
            plugins_ = probePlugins(pluginList, callback, valid_);
            for (auto& desc : plugins_){
                pluginMap_[desc->name] = desc;
            }
        };
    } else {
        auto f = probePlugin(pluginList_[0]);
        return [this, self=std::move(self), f=std::move(f)](ProbeCallback callback){
            auto result = f();
            plugins_ = { result };
            valid_ = result->valid();
            /// LOG_DEBUG("probed plugin " << result->name);
            if (callback){
                callback(*result, 0, 1);
            }
            pluginMap_[result->name] = result;
        };
    }
}

std::unique_ptr<IPlugin> VST3Factory::create(const std::string& name, bool probe) const {
    PluginInfo::ptr desc = nullptr; // will stay nullptr when probing!
    if (!probe){
        if (plugins_.empty()){
            throw Error("factory doesn't have any plugin(s)");
        }
        auto it = pluginMap_.find(name);
        if (it == pluginMap_.end()){
            throw Error("can't find (sub)plugin '" + name + "'");
        }
        desc = it->second;
        if (!desc->valid()){
            throw Error("plugin not probed successfully");
        }
    }
    return std::make_unique<VST3Plugin>(factory_, pluginIndexMap_[name], shared_from_this(), desc);
}

/*///////////////////// ParamValueQeue /////////////////////*/

ParamValueQueue::ParamValueQueue() {
    values_.reserve(defaultNumPoints);
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
    // start from the end because we likely add values in "chronological" order
    for (auto it = values_.end(); it != values_.begin(); --it){
        if (sampleOffset > it->sampleOffset){
            break;
        } else if (sampleOffset == it->sampleOffset){
            index = it - values_.begin();
            return kResultOk;
        } else {
            values_.emplace(it, value, sampleOffset);
            return kResultOk;
        }
    }
    index = values_.size();
    values_.emplace_back(value, sampleOffset);
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
    events_.reserve(defaultNumEvents);
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
    events_.push_back(e);
    return kResultOk;
}

void EventList::addSysexEvent(const SysexEvent& event){
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

#if HAVE_NRT_THREAD
#define LOCK_GUARD std::lock_guard<std::mutex> LOCK_GUARD##_lock(mutex_);
#else
#define LOCK_GUARD
#endif

#if HAVE_NRT_THREAD
#define LOCK mutex_.lock();
#else
#define LOCK
#endif

#if HAVE_NRT_THREAD
#define UNLOCK mutex_.unlock();
#else
#define UNLOCK
#endif

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
    : factory_(std::move(f)), info_(std::move(desc))
{
    memset(audioInput_, 0, sizeof(audioInput_));
    memset(audioOutput_, 0, sizeof(audioOutput_));
    memset(&context_, 0, sizeof(context_));
    context_.state = Vst::ProcessContext::kPlaying | Vst::ProcessContext::kContTimeValid
            | Vst::ProcessContext::kProjectTimeMusicValid | Vst::ProcessContext::kBarPositionValid
            | Vst::ProcessContext::kCycleValid | Vst::ProcessContext::kTempoValid
            | Vst::ProcessContext::kTimeSigValid | Vst::ProcessContext::kClockValid;
    context_.sampleRate = 1;
    context_.tempo = 120;
    context_.timeSigNumerator = 4;
    context_.timeSigDenominator = 4;

    // are we probing?
    auto info = !info_ ? std::make_shared<PluginInfo>(factory_) : nullptr;
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
            throw Error("couldn't get class info!");
        }
    }
    if (info){
        LOG_DEBUG("creating " << info->name);
    }
    // create component
    if (!(component_ = createInstance<Vst::IComponent>(factory, uid))){
        throw Error("couldn't create VST3 component");
    }
    LOG_DEBUG("created VST3 component");
    // initialize component
    if (component_->initialize(getHostContext()) != kResultOk){
        throw Error("couldn't initialize VST3 component");
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
                throw Error("couldn't initialize VST3 controller");
            }
        }
    }
    if (controller_){
        LOG_DEBUG("created VST3 controller");
    } else {
        throw Error("couldn't get VST3 controller!");
    }
    if (controller_->setComponentHandler(this) != kResultOk){
        throw Error("couldn't set component handler");
    }
    FUnknownPtr<Vst::IConnectionPoint> componentCP(component_);
    FUnknownPtr<Vst::IConnectionPoint> controllerCP(controller_);
    // connect component and controller
    if (componentCP && controllerCP){
    #if 0
        componentCP->connect(controllerCP);
        controllerCP->connect(componentCP);
    #else
        // use *this* as a proxy
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
        throw Error("couldn't get VST3 processor");
    }
    // check
    // get IO channel count
    auto getChannelCount = [this](auto media, auto dir, auto type) -> int {
        auto count = component_->getBusCount(media, dir);
        for (int i = 0; i < count; ++i){
            Vst::BusInfo bus;
            if (component_->getBusInfo(media, dir, i, bus) == kResultTrue
                && bus.busType == type)
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
    audioInput_[Main].numChannels = getChannelCount(Vst::kAudio, Vst::kInput, Vst::kMain);
    audioInput_[Aux].numChannels = getChannelCount(Vst::kAudio, Vst::kInput, Vst::kAux);
    audioOutput_[Main].numChannels = getChannelCount(Vst::kAudio, Vst::kOutput, Vst::kMain);
    audioOutput_[Aux].numChannels = getChannelCount(Vst::kAudio, Vst::kOutput, Vst::kAux);
    numMidiInChannels_ = getChannelCount(Vst::kEvent, Vst::kInput, Vst::kMain);
    numMidiOutChannels_ = getChannelCount(Vst::kEvent, Vst::kOutput, Vst::kMain);
    LOG_DEBUG("in: " << getNumInputs() << ", auxin: " << getNumAuxInputs());
    LOG_DEBUG("out: " << getNumOutputs() << ", auxout: " << getNumAuxOutputs());
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
        info->numInputs = getNumInputs();
        info->numAuxInputs = getNumAuxInputs();
        info->numOutputs = getNumOutputs();
        info->numAuxOutputs = getNumAuxOutputs();
        uint32_t flags = 0;
        flags |= hasEditor() * PluginInfo::HasEditor;
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
                param.name = StringConvert::convert(pi.title);
                param.label = StringConvert::convert(pi.units);
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
        auto ui = FUnknownPtr<Vst::IUnitInfo>(controller);
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
                            info->programs.push_back(StringConvert::convert(name));
                        } else {
                            LOG_ERROR("couldn't get program name!");
                            info->programs.push_back("");
                        }
                    }
                } else {
                    LOG_ERROR("couldn't get program list info");
                }
            }
        }
        info_ = info;
    }
    // setup parameter queues/cache
    int numParams = info_->numParameters();
    inputParamChanges_.setMaxNumParameters(numParams);
    // outputParamChanges_.setMaxNumParameters(numParams);
    paramCache_.resize(numParams);
    for (int i = 0; i < numParams; ++i){
        auto id = info_->getParamID(i);
        paramCache_[i] = controller_->getParamNormalized(id);
    }
    LOG_DEBUG("program change: " << info_->programChange);
    LOG_DEBUG("bypass: " << info_->bypass);
}

VST3Plugin::~VST3Plugin(){
    processor_ = nullptr;
    controller_->terminate();
    controller_ = nullptr;
    component_->terminate();
}

// IComponentHandler
tresult VST3Plugin::beginEdit(Vst::ParamID id){
    LOG_DEBUG("begin edit");
    return kResultOk;
}

tresult VST3Plugin::performEdit(Vst::ParamID id, Vst::ParamValue value){
    auto listener = listener_.lock();
    if (listener){
        listener->parameterAutomated(info().getParamIndex(id), value);
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
    if (window_){
        // TODO
    } else {
        sendMessage(message);
    }
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
}

void VST3Plugin::process(ProcessData<float>& data){
    // buffers
    audioInput_[Main].channelBuffers32 = (float **)data.input;
    audioInput_[Aux].channelBuffers32 = (float **)data.auxInput;
    audioOutput_[Main].channelBuffers32 = data.output;
    audioOutput_[Aux].channelBuffers32 = data.auxOutput;
    // process data
    Vst::ProcessData processData;
    processData.symbolicSampleSize = Vst::kSample32;
    processData.numSamples = data.numSamples;
    processData.numInputs = data.auxInput ? 2 : 1;
    processData.numOutputs = data.auxOutput ? 2 : 1;
    doProcess(processData);
}

void VST3Plugin::process(ProcessData<double>& data){
    // buffers
    audioInput_[Main].channelBuffers64 = (double **)data.input;
    audioInput_[Aux].channelBuffers64 = (double **)data.auxInput;
    audioOutput_[Main].channelBuffers64 = data.output;
    audioOutput_[Aux].channelBuffers64 = data.auxOutput;
    // process data
    Vst::ProcessData processData;
    processData.symbolicSampleSize = Vst::kSample64;
    processData.numSamples = data.numSamples;
    processData.numInputs = data.auxInput ? 2 : 1;
    processData.numOutputs = data.auxOutput ? 2 : 1;
    doProcess(processData);
}

void VST3Plugin::doProcess(Vst::ProcessData &data){
    // setup process data
    data.inputs = audioInput_;
    data.outputs = audioOutput_;
    data.processContext = &context_;
    data.inputEvents = &inputEvents_;
    data.outputEvents = &outputEvents_;
    data.inputParameterChanges = &inputParamChanges_;
    // data.outputParameterChanges = &outputParamChanges_;

    // process
    LOCK
    processor_->process(data);
    UNLOCK

    // clear input queues
    inputEvents_.clear();
    inputParamChanges_.clear();

    // handle outgoing events
    handleEvents();

    // update time info
    context_.continousTimeSamples += data.numSamples;
    context_.projectTimeSamples += data.numSamples;
    // advance project time in bars
    float sec = data.numSamples / context_.sampleRate;
    float numBeats = sec * context_.tempo / 60.f;
    context_.projectTimeMusic += numBeats;
    // bar start: simply round project time (in bars) down to bar length
    float barLength = context_.timeSigNumerator * context_.timeSigDenominator / 4.0;
    context_.barPositionMusic = static_cast<int64_t>(context_.projectTimeMusic / barLength) * barLength;
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
    LOCK_GUARD
    processor_->setProcessing(false);
    component_->setActive(false);
}

void VST3Plugin::resume(){
    LOCK_GUARD
    component_->setActive(true);
    processor_->setProcessing(true);
}

int VST3Plugin::getNumInputs() const {
    return audioInput_[Main].numChannels;
}

int VST3Plugin::getNumAuxInputs() const {
    return audioInput_[Aux].numChannels;
}

int VST3Plugin::getNumOutputs() const {
    return audioOutput_[Main].numChannels;
}

int VST3Plugin::getNumAuxOutputs() const {
    return audioOutput_[Aux].numChannels;
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

void VST3Plugin::setBypass(bool bypass){
    auto id = info().bypass;
    if (id != PluginInfo::NoParamID){
        doSetParameter(id, bypass);
    } else {
        LOG_DEBUG("no bypass parameter");
    }
}

#define makeChannels(n) ((1 << (n)) - 1)

void VST3Plugin::setNumSpeakers(int in, int out, int auxIn, int auxOut){
    // input busses
    Vst::SpeakerArrangement busIn[64] = { 0 };
    auto numIn = component_->getBusCount(Vst::kAudio, Vst::kInput);
    for (int i = 0; i < numIn; ++i){
        Vst::BusInfo bus;
        if (component_->getBusInfo(Vst::kAudio, Vst::kInput, i, bus) == kResultTrue){
            if (bus.busType == Vst::kMain && i == 0){
                busIn[i] = makeChannels(in);
                LOG_DEBUG("main input index: " << i);
            } else if (bus.busType == Vst::kAux && i == 1){
                busIn[i] = makeChannels(auxIn);
                LOG_DEBUG("aux input index: " << i);
            }
        }
    }
    // output busses
    Vst::SpeakerArrangement busOut[64] = { 0 };
    auto numOut = component_->getBusCount(Vst::kAudio, Vst::kOutput);
    for (int i = 0; i < numOut; ++i){
        Vst::BusInfo bus;
        if (component_->getBusInfo(Vst::kAudio, Vst::kInput, i, bus) == kResultTrue){
            if (bus.busType == Vst::kMain && i == 0){
                busOut[i] = makeChannels(out);
                LOG_DEBUG("main output index: " << i);
            } else if (bus.busType == Vst::kAux && i == 1){
                busOut[i] = makeChannels(auxOut);
                LOG_DEBUG("aux output index: " << i);
            }
        }
    }
    LOCK_GUARD
    processor_->setBusArrangements(busIn, numIn, busOut, numOut);
    // we have to activate busses *after* setting the bus arrangement
    component_->activateBus(Vst::kAudio, Vst::kInput, 0, in > 0); // main
    component_->activateBus(Vst::kAudio, Vst::kInput, 1, auxIn > 0); // aux
    component_->activateBus(Vst::kAudio, Vst::kOutput, 0, out > 0); // main
    component_->activateBus(Vst::kAudio, Vst::kOutput, 1, auxOut > 0); // aux
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
        // TODO ?
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
    e.sampleOffset = 0;
    e.ppqPosition = context_.projectTimeMusic;
    e.flags = Vst::Event::kIsLive;
    auto status = event.data[0] & 0xf0;
    auto channel = event.data[0] & 0x0f;
    auto data1 = event.data[1] & 127;
    auto data2 = event.data[2] & 127;
    auto value = (float)data2 / 127.f;
    switch (status){
    case 0x80: // note off
        e.type = Vst::Event::kNoteOffEvent;
        e.noteOff.channel = channel;
        e.noteOff.noteId = -1;
        e.noteOff.pitch = data1;
        e.noteOff.velocity = value;
        e.noteOff.tuning = 0;
        break;
    case 0x90: // note on
        e.type = Vst::Event::kNoteOnEvent;
        e.noteOn.channel = channel;
        e.noteOn.noteId = -1;
        e.noteOn.pitch = data1;
        e.noteOn.velocity = value;
        e.noteOn.tuning = 0;
        e.noteOn.length = 0;
        break;
    case 0xa0: // polytouch
        e.type = Vst::Event::kPolyPressureEvent;
        e.polyPressure.channel = channel;
        e.polyPressure.pitch = data1;
        e.polyPressure.pressure = value;
        e.polyPressure.noteId = -1;
        break;
    case 0xb0: // CC
        {
            Vst::ParamID id = Vst::kNoParamId;
            FUnknownPtr<Vst::IMidiMapping> midiMapping(controller_);
            if (midiMapping && midiMapping->getMidiControllerAssignment (0, channel, data1, id) == kResultOk){
                doSetParameter(id, value);
            } else {
                LOG_WARNING("MIDI CC control number " << data1 << " not supported");
            }
            return;
        }
    case 0xc0: // program change
        setProgram(event.data[1]);
        return;
    case 0xd0: // channel aftertouch
        {
            Vst::ParamID id = Vst::kNoParamId;
            FUnknownPtr<Vst::IMidiMapping> midiMapping(controller_);
            if (midiMapping && midiMapping->getMidiControllerAssignment (0, channel, Vst::kAfterTouch, id) == kResultOk){
                doSetParameter(id, (float)data2 / 127.f);
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
                doSetParameter(id, (float)bend / 16383.f);
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
    paramCache_[index] = value;
}

bool VST3Plugin::setParameter(int index, const std::string &str, int sampleOffset){
    Vst::ParamValue value;
    Vst::String128 string;
    auto id = info().getParamID(index);
    if (StringConvert::convert(str, string)){
        if (controller_->getParamValueByString(id, string, value) == kResultOk){
            doSetParameter(id, value, sampleOffset);
            paramCache_[index] = value;
            return true;
        }
    }
    return false;
}

void VST3Plugin::doSetParameter(Vst::ParamID id, float value, int32 sampleOffset){
    int32 dummy;
    inputParamChanges_.addParameterData(id, dummy)->addPoint(sampleOffset, value, dummy);
    if (window_){
        // The VST3 guys say that you must only call IEditController methods on the GUI thread!
        // This means we have to transfer parameter changes to a ring buffer and install a timer
        // on the GUI thread which polls the ring buffer and calls the edit controller.
        // Similarly, we have to have a ringbuffer for parameter changes coming from the GUI.
    } else {
        controller_->setParamNormalized(id, value);
    }
}

float VST3Plugin::getParameter(int index) const {
    return paramCache_[index];
}

std::string VST3Plugin::getParameterString(int index) const {
    Vst::String128 display;
    auto id = info().getParamID(index);
    auto value = paramCache_[index];
    if (controller_->getParamStringByValue(id, value, display) == kResultOk){
        return StringConvert::convert(display);
    }
    return std::string{};
}

int VST3Plugin::getNumParameters() const {
    return info().numParameters();
}

void VST3Plugin::setProgram(int program){
    auto id = info().programChange;
    if (id != PluginInfo::NoParamID){
        if (program >= 0 && program < getNumPrograms()){
            auto value = controller_->plainParamToNormalized(id, program);
            doSetParameter(id, value);
            program_ = program;
        } else {
            LOG_WARNING("program number out of range!");
        }
    } else {
        LOG_DEBUG("no program change parameter");
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
    Vst::ChunkID id;
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
    #if LOGLEVEL > 2
        char buf[17] = {0};
        memcpy(buf, classID, sizeof(TUID));
        LOG_DEBUG("a: " << buf);
        memcpy(buf, info().getUID(), sizeof(TUID));
        LOG_DEBUG("b: " << buf);
    #endif
        throw Error("wrong class ID");
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
        LOCK_GUARD
        if (isChunkType(entry.id, Vst::kComponentState)){
            if (component_->setState(&stream) == kResultOk){
                // also update controller state!
                stream.setPos(entry.offset); // rewind
                if (window_){
                    // TODO ?
                } else {
                    controller_->setComponentState(&stream);
                }
                LOG_DEBUG("restored component state");
            } else {
                LOG_WARNING("couldn't restore component state");
            }
        } else if (isChunkType(entry.id, Vst::kControllerState)){
            if (window_){
                // TODO ?
            } else {
                if (controller_->setState(&stream) == kResultOk){
                    LOG_DEBUG("restored controller set");
                } else {
                    LOG_WARNING("couldn't restore controller state");
                }
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
    auto writeData = [&](auto component, Vst::ChunkType type){
        ChunkListEntry entry;
        memcpy(entry.id, Vst::getChunkID(type), sizeof(Vst::ChunkID));
        stream.tell(&entry.offset);
        LOCK_GUARD
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
    writeData(component_, Vst::kComponentState);
    writeData(controller_, Vst::kControllerState);
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
    return false;
}

void VST3Plugin::openEditor(void * window){

}

void VST3Plugin::closeEditor(){

}

void VST3Plugin::getEditorRect(int &left, int &top, int &right, int &bottom) const {

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
        StringConvert::convert(value, buf);
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
        LOCK_GUARD
        p1->notify(msg);
    }
    FUnknownPtr<Vst::IConnectionPoint> p2(controller_);
    if (p2){
        if (window_){
            // TODO ?
        } else {
            LOCK_GUARD
            p2->notify(msg);
        }
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
        sprintf(buf + (i * 2), "%02X", tuid[i]);
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
    StringConvert::convert("vstplugin~", name);
#else
    StringConvert::convert("VSTPlugin", name);
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
    other.size = 0;
    other.v.b = nullptr;
}

HostAttribute::~HostAttribute(){
    if (size > 0){
        delete[] v.b;
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
    if (attr && attr->type == HostAttribute::kString){
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
            LOG_VERBOSE(id << ": " << StringConvert::convert(attr.v.s));
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
    return attributes_.get();
}

void HostMessage::print(){
    if (attributes_){
        attributes_->print();
    }
}

} // vst
