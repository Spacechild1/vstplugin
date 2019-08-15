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
    : path_(path), module_(IModule::load(path))
{
    if (!module_){
        // shouldn't happen...
        throw Error("couldn't load module!");
    }
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

/*///////////////////// VST3Plugin /////////////////////*/

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
    // LATER safe this in PluginInfo
    memcpy(uid_, uid, sizeof(TUID));
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
        componentCP->connect(controllerCP);
        controllerCP->connect(componentCP);
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
    auto getChannelCount = [this](auto media, auto dir, auto type) {
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
    // finally get remaining info
    if (info){
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
        info->flags_ = flags;
        // get parameters
        std::set<Vst::ParamID> params;
        int numParameters = controller_->getParameterCount();
        int index = 0;
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
                        // inverse mapping
                        info->paramMap_[param.name] = index;
                        // index -> ID mapping
                        info->indexToIdMap_[index] = param.id;
                        // ID -> index mapping
                        info->idToIndexMap_[param.id] = index;
                        // add parameter
                        info->parameters.push_back(std::move(param));
                        index++;
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
    outputParamChanges_.setMaxNumParameters(numParams);
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
#define PRINT_FLAG(name) if (flags & name) LOG_DEBUG(#name)
    PRINT_FLAG(Vst::kReloadComponent);
    PRINT_FLAG(Vst::kIoChanged);
    PRINT_FLAG(Vst::kParamValuesChanged); 										///< (as result of a program change for example)  [SDK 3.0.0]
    PRINT_FLAG(Vst::kLatencyChanged);
    PRINT_FLAG(Vst::kParamTitlesChanged);
    PRINT_FLAG(Vst::kMidiCCAssignmentChanged);
    PRINT_FLAG(Vst::kNoteExpressionChanged);
    PRINT_FLAG(Vst::kIoTitlesChanged);
    PRINT_FLAG(Vst::kPrefetchableSupportChanged);
    PRINT_FLAG(Vst::kRoutingInfoChanged);
#undef PRINT_FLAG
    return kResultOk;
}

int VST3Plugin::canDo(const char *what) const {
    return 0;
}

intptr_t VST3Plugin::vendorSpecific(int index, intptr_t value, void *p, float opt){
    return 0;
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
    data.outputParameterChanges = &outputParamChanges_;
    // process
    processor_->process(data);
    // clear input queues
    inputEvents_.clear();
    inputParamChanges_.clear();
    // flush and clear output queues
    outputEvents_.clear();
    outputParamChanges_.clear();
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

bool VST3Plugin::hasPrecision(ProcessPrecision precision) const {
    switch (precision){
    case ProcessPrecision::Single:
        return processor_->canProcessSampleSize(Vst::kSample32) == kResultOk;
    case ProcessPrecision::Double:
        return processor_->canProcessSampleSize(Vst::kSample64) == kResultOk;
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
    return false;
}

void VST3Plugin::setBypass(bool bypass){

}

#define makeChannels(n) ((1 << (n)) - 1)

void VST3Plugin::setNumSpeakers(int in, int out, int auxIn, int auxOut){
    // input busses
    Vst::SpeakerArrangement busIn[64] = { 0 };
    auto numIn = component_->getBusCount(Vst::kAudio, Vst::kInput);
    for (int i = 0; i < numIn; ++i){
        Vst::BusInfo bus;
        if (component_->getBusInfo(Vst::kAudio, Vst::kInput, i, bus) == kResultTrue){
            if (bus.busType == Vst::kMain){
                busIn[i] = makeChannels(in);
            } else if (bus.busType == Vst::kAux){
                busIn[i] = makeChannels(auxIn);
            }
        }
    }
    // output busses
    Vst::SpeakerArrangement busOut[64] = { 0 };
    auto numOut = component_->getBusCount(Vst::kAudio, Vst::kOutput);
    for (int i = 0; i < numOut; ++i){
        Vst::BusInfo bus;
        if (component_->getBusInfo(Vst::kAudio, Vst::kInput, i, bus) == kResultTrue){
            if (bus.busType == Vst::kMain){
                busOut[i] = makeChannels(out);
            } else if (bus.busType == Vst::kAux){
                busOut[i] = makeChannels(auxOut);
            }
        }
    }
    processor_->setBusArrangements(busIn, numIn, busOut, numOut);
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

}

void VST3Plugin::setTransportAutomationReading(bool reading){

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

}

void VST3Plugin::sendSysexEvent(const SysexEvent &event){

}

void VST3Plugin::setParameter(int index, float value){
    int32 dummy;
    auto id = info().getParamID(index);
    inputParamChanges_.addParameterData(id, dummy)->addPoint(0, value, dummy);
    paramCache_[index] = value;
}

bool VST3Plugin::setParameter(int index, const std::string &str){
    int32 dummy;
    Vst::ParamValue value;
    Vst::String128 string;
    auto id = info().getParamID(index);
    if (StringConvert::convert(str, string)){
        if (controller_->getParamValueByString(id, string, value) == kResultOk){
            inputParamChanges_.addParameterData(id, dummy)->addPoint(0, value, dummy);
            paramCache_[index] = value;
            return true;
        }
    }
    return false;
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
    return paramCache_.size();
}

void VST3Plugin::setProgram(int program){
}

void VST3Plugin::setProgramName(const std::string& name){
}

int VST3Plugin::getProgram() const {
    return 0;
}

std::string VST3Plugin::getProgramName() const {
    return std::string{};
}

std::string VST3Plugin::getProgramNameIndexed(int index) const {
    return std::string{};
}

int VST3Plugin::getNumPrograms() const {
    return 0;
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
    if (memcmp(classID, uid_, sizeof(TUID)) != 0){
    #if LOGLEVEL > 2
        char buf[17] = {0};
        memcpy(buf, classID, sizeof(TUID));
        LOG_DEBUG("a: " << buf);
        memcpy(buf, uid_, sizeof(TUID));
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
        if (isChunkType(entry.id, Vst::kComponentState)){
            if (component_->setState(&stream) == kResultOk){
                // also update controller state!
                stream.setPos(entry.offset); // rewind
                controller_->setComponentState(&stream);
                LOG_DEBUG("restored component state");
            } else {
                LOG_WARNING("couldn't restore component state");
            }
        } else if (isChunkType(entry.id, Vst::kControllerState)){
            if (controller_->setState(&stream) == kResultOk){
                LOG_DEBUG("restored controller set");
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
    stream.writeTUID(uid_); // class ID
    stream.writeInt64(0); // skip offset
    // write data
    auto writeData = [&](auto component, Vst::ChunkType type){
        ChunkListEntry entry;
        memcpy(entry.id, Vst::getChunkID(type), sizeof(Vst::ChunkID));
        stream.tell(&entry.offset);
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
    LOG_DEBUG("BaseStream: read " << numBytes << " bytes");
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
    LOG_DEBUG("BaseStream: set cursor to " << cursor_);
    return kResultTrue;
}

tresult BaseStream::tell  (int64* pos){
    if (pos){
        *pos = cursor_;
        LOG_DEBUG("BaseStream: told cursor pos");
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
    int bytesWritten = 0;
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
    int bytesWritten = 0;
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
    int bytesRead = 0;
    read((void *)id, sizeof(Vst::ChunkID), &bytesRead);
    return bytesRead == sizeof(Vst::ChunkID);
}

bool BaseStream::readTUID(TUID tuid){
    int bytesRead = 0;
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
    LOG_DEBUG("BaseStream: wrote " << numBytes << " bytes");
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
    // addInterface(Vst::IMidiMapping::iid);

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
    // addInterface(Vst::IAutomationState::iid);

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
    LOG_DEBUG("host: createInstance");
    if (classID == Vst::IMessage::iid && interfaceID == Vst::IMessage::iid)
    {
        LOG_DEBUG("create HostMessage");
        *obj = (Vst::IMessage *)new HostMessage;
        return kResultTrue;
    }
    else if (classID == Vst::IAttributeList::iid && interfaceID == Vst::IAttributeList::iid)
    {
        LOG_DEBUG("create HostAttributeList");
        *obj = (Vst::IAttributeList *)new HostAttributeList;
        return kResultTrue;
    }
    *obj = nullptr;
    return kResultFalse;
}

tresult PLUGIN_API HostApplication::queryInterface (const char* _iid, void** obj){
    LOG_DEBUG("host: query interface");
    QUERY_INTERFACE (_iid, obj, FUnknown::iid, IHostApplication)
    QUERY_INTERFACE (_iid, obj, IHostApplication::iid, IHostApplication)
    if (interfaceSupport_ && interfaceSupport_->queryInterface (iid, obj) == kResultTrue)
        return kResultOk;

    *obj = nullptr;
    return kResultFalse;
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
    list_.emplace(aid, HostAttribute(string, wcslen((const wchar_t *)string)));
    return kResultTrue;
}

tresult PLUGIN_API HostAttributeList::getString (AttrID aid, Vst::TChar* string, uint32 size){
    auto attr = find(aid);
    if (attr && attr->type == HostAttribute::kString){
        size = std::min<uint32>(size-1, attr->size);
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

/*///////////////////// HostMessage //////////////////////////*/

Vst::IAttributeList* PLUGIN_API HostMessage::getAttributes () {
    LOG_DEBUG("HostMessage::getAttributes");
    if (!attributeList_){
        attributeList_.reset(new HostAttributeList);
    }
    return attributeList_.get();
}

} // vst
