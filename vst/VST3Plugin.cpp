#include "VST3Plugin.h"
#include "Utility.h"

#include <cstring>
#include <algorithm>

DEF_CLASS_IID (IPluginBase)
DEF_CLASS_IID (IPlugView)
// DEF_CLASS_IID (IPlugFrame)
DEF_CLASS_IID (IPluginFactory)
DEF_CLASS_IID (IPluginFactory2)
DEF_CLASS_IID (IPluginFactory3)
DEF_CLASS_IID (Vst::IComponent)
DEF_CLASS_IID (Vst::IEditController)
DEF_CLASS_IID (Vst::IAudioProcessor)
DEF_CLASS_IID (Vst::IUnitInfo)

#ifndef HAVE_VST3_BASE
#define HAVE_VST3_BASE 0
#endif
#if HAVE_VST3_BASE
#include "base/source/fstring.h"
#else
#include <codecvt>
#include <locale>
using t_string_converter = std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>;
#endif

namespace vst {
#if HAVE_VST3_BASE
std::string fromString128(const Vst::String128 s){
    std::string result;
    int bytes = ConstString::wideStringToMultiByte(0, s, 0);
    if (bytes){
        bytes += 1;
        result.resize(bytes);
        ConstString::wideStringToMultiByte(&result[0], s, bytes);
    }
    return result;
}

bool toString128(const std::string& s, Vst::String128 ws){
    int bytes = ConstString::multiByteToWideString(0, s.data(), 0);
    if (bytes){
        bytes += sizeof(Vst::TChar);
        if (ConstString::multiByteToWideString(ws, s.data(), s.size() + 1) > 0){
            return true;
        }
    }
    return false;
}
#else // HAVE_VST3_BASE
#if 1
t_string_converter& string_converter(){
    static t_string_converter c;
    return c;
}

std::string fromString128(const Vst::String128 s){
    try {
        return string_converter().to_bytes((const wchar_t *)s);
    } catch (...){
        return "";
    }
}
// bash to wide characters (very bad)
bool toString128(const std::string& s, Vst::String128 dest){
    try {
        auto ws = string_converter().from_bytes(s);
        auto n = std::min<int>(ws.size(), 127);
        memcpy(dest, ws.data(), n);
        dest[n] = 0;
        return true;
    } catch (...){
        return false;
    }
}
#else
// bash to ASCII (bad)
std::string fromString128(const Vst::String128 s){
    char buf[128];
    auto from = s;
    auto to = buf;
    Vst::TChar c;
    while ((c = *from++)){
        if (c < 128){
            *to++ = c;
        } else {
            *to++ = '?';
        }
    }
    *to = 0; // null terminate!
    return buf;
}
// bash to wide characters (very bad)
bool toString128(const std::string& s, Vst::String128 ws){
    int size = s.size();
    for (int i = 0; i < size; ++i){
        ws[i] = s[i];
    }
    ws[size] = 0;
    return true;
}
#endif
#endif // HAVE_VST3_BASE

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

/*/////////////////////// VST3Plugin /////////////////////////////*/

template <typename T>
inline IPtr<T> createInstance (IPtr<IPluginFactory> factory, TUID iid){
    T* obj = nullptr;
    if (factory->createInstance (iid, T::iid, reinterpret_cast<void**> (&obj)) == kResultTrue){
        return owned(obj);
    } else {
        return nullptr;
    }
}

static FUnknown *gPluginContext = nullptr;

VST3Plugin::VST3Plugin(IPtr<IPluginFactory> factory, int which, IFactory::const_ptr f, PluginInfo::const_ptr desc)
    : factory_(std::move(f)), info_(std::move(desc))
{
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
    // create component
    if (!(component_ = createInstance<Vst::IComponent>(factory, uid))){
        throw Error("couldn't create VST3 component");
    }
    LOG_DEBUG("created VST3 component");
    // initialize component
    if (component_->initialize(gPluginContext) != kResultOk){
        throw Error("couldn't initialize VST3 component");
    }
    // first try to create controller from the component part
    auto controller = FUnknownPtr<Vst::IEditController>(component_);
    if (controller){
        controller_ = shared(controller.getInterface());
    } else {
        // if this fails, try to instantiate controller class
        TUID controllerCID;
        if (component_->getControllerClassId(controllerCID) == kResultTrue){
            controller_ = createInstance<Vst::IEditController>(factory, controllerCID);
        }
    }
    if (controller_){
        LOG_DEBUG("created VST3 controller");
    } else {
        throw Error("couldn't get VST3 controller!");
    }
    if (controller_->initialize(gPluginContext) != kResultOk){
        throw Error("couldn't initialize VST3 controller");
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
                && bus.busType == type){
                return std::make_pair(bus.channelCount, i);
            }
        }
        return std::make_pair(0, -1);
    };
    std::tie(numInputs_, inputIndex_) = getChannelCount(Vst::kAudio, Vst::kInput, Vst::kMain);
    std::tie(numAuxInputs_, auxInputIndex_) = getChannelCount(Vst::kAudio, Vst::kInput, Vst::kAux);
    std::tie(numOutputs_, outputIndex_) = getChannelCount(Vst::kAudio, Vst::kOutput, Vst::kMain);
    std::tie(numAuxOutputs_, auxOutputIndex_) = getChannelCount(Vst::kAudio, Vst::kOutput, Vst::kAux);
    std::tie(numMidiInChannels_, midiInIndex_) = getChannelCount(Vst::kEvent, Vst::kInput, Vst::kMain);
    std::tie(numMidiOutChannels_, midiOutIndex_) = getChannelCount(Vst::kEvent, Vst::kOutput, Vst::kMain);
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
        info->numOutputs = getNumOutputs();
        uint32_t flags = 0;
        flags |= hasEditor() * PluginInfo::HasEditor;
        flags |= (info->category.find(Vst::PlugType::kInstrument) != std::string::npos) * PluginInfo::IsSynth;
        flags |= hasPrecision(ProcessPrecision::Single) * PluginInfo::SinglePrecision;
        flags |= hasPrecision(ProcessPrecision::Double) * PluginInfo::DoublePrecision;
        flags |= hasMidiInput() * PluginInfo::MidiInput;
        flags |= hasMidiOutput() * PluginInfo::MidiOutput;
        info->flags_ = flags;
        // get parameters
        int numParameters = controller_->getParameterCount();
        for (int i = 0; i < numParameters; ++i){
            PluginInfo::Param param;
            Vst::ParameterInfo pi;
            if (controller_->getParameterInfo(i, pi) == kResultTrue){
                param.name = fromString128(pi.title);
                param.label = fromString128(pi.units);
                param.id = pi.id;
            } else {
                LOG_ERROR("couldn't get parameter info!");
            }
            // inverse mapping
            info->paramMap[param.name] = i;
            // index -> ID mapping
            info->paramIDMap[i] = param.id;
            // add parameter
            info->parameters.push_back(std::move(param));
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
                // for now, just the program list for root unit (0)
                if (ui->getProgramListInfo(0, pli) == kResultTrue){
                    for (int i = 0; i < pli.programCount; ++i){
                        Vst::String128 name;
                        if (ui->getProgramName(pli.id, i, name) == kResultTrue){
                            info->programs.push_back(fromString128(name));
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
}

VST3Plugin::~VST3Plugin(){
    processor_ = nullptr;
    controller_->terminate();
    controller_ = nullptr;
    component_->terminate();
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
}

void VST3Plugin::process(ProcessData<float>& data){

}

void VST3Plugin::process(ProcessData<double>& data){

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
}

void VST3Plugin::resume(){
    processor_->setProcessing(true);
}

int VST3Plugin::getNumInputs() const {
    return numInputs_;
}

int VST3Plugin::getNumAuxInputs() const {
    return numAuxInputs_;
}

int VST3Plugin::getNumOutputs() const {
    return numOutputs_;
}

int VST3Plugin::getNumAuxOutputs() const {
    return numAuxOutputs_;
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

#define setbits(x, n) (x) |= ((1 << n) - 1)

void VST3Plugin::setNumSpeakers(int in, int out, int auxIn, int auxOut){
    Vst::SpeakerArrangement busIn[64] = { 0 };
    Vst::SpeakerArrangement busOut[64] = { 0 };
    int numIn = 0;
    int numOut = 0;
    // LATER get real bus indices
    if (inputIndex_ >= 0){
        setbits(busIn[inputIndex_], in);
        numIn = inputIndex_ + 1;
    }
    if (auxInputIndex_ >= 0){
        setbits(busIn[auxInputIndex_], auxIn);
        numIn = auxInputIndex_ + 1;
    }
    if (outputIndex_ >= 0){
        setbits(busOut[outputIndex_], out);
        numOut = outputIndex_ + 1;
    }
    if (auxOutputIndex_ >= 0){
        setbits(busOut[auxOutputIndex_], auxOut);
        numOut = auxOutputIndex_ + 1;
    }
    processor_->setBusArrangements(busIn, numIn, busOut, numOut);
}

void VST3Plugin::setTempoBPM(double tempo){
}

void VST3Plugin::setTimeSignature(int numerator, int denominator){

}

void VST3Plugin::setTransportPlaying(bool play){

}

void VST3Plugin::setTransportRecording(bool record){

}

void VST3Plugin::setTransportAutomationWriting(bool writing){

}

void VST3Plugin::setTransportAutomationReading(bool reading){

}

void VST3Plugin::setTransportCycleActive(bool active){

}

void VST3Plugin::setTransportCycleStart(double beat){

}

void VST3Plugin::setTransportCycleEnd(double beat){

}

void VST3Plugin::setTransportPosition(double beat){

}

double VST3Plugin::getTransportPosition() const {
    return 0;
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
    auto it = info_->paramIDMap.find(index);
    if (it != info_->paramIDMap.end()){
        controller_->setParamNormalized(it->second, value);
    }
}

bool VST3Plugin::setParameter(int index, const std::string &str){
    Vst::ParamValue value;
    Vst::String128 string;
    auto it = info_->paramIDMap.find(index);
    if (it != info_->paramIDMap.end()){
        auto id = it->second;
        if (toString128(str, string)){
            if (controller_->getParamValueByString(id, string, value) == kResultOk){
                return controller_->setParamNormalized(id, value) == kResultOk;
            }
        }
    }
    return false;
}

float VST3Plugin::getParameter(int index) const {
    auto it = info_->paramIDMap.find(index);
    if (it != info_->paramIDMap.end()){
        return controller_->getParamNormalized(it->second);
    } else {
        return -1;
    }
}

std::string VST3Plugin::getParameterString(int index) const {
    Vst::String128 display;
    auto it = info_->paramIDMap.find(index);
    if (it != info_->paramIDMap.end()){
        auto id = it->second;
        auto value = controller_->getParamNormalized(id);
        if (controller_->getParamStringByValue(id, value, display) == kResultOk){
            return fromString128(display);
        }
    }
    return std::string{};
}

int VST3Plugin::getNumParameters() const {
    return controller_->getParameterCount();
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

bool VST3Plugin::hasChunkData() const {
    return false;
}

void VST3Plugin::setProgramChunkData(const void *data, size_t size){
}

void VST3Plugin::getProgramChunkData(void **data, size_t *size) const {
}

void VST3Plugin::setBankChunkData(const void *data, size_t size){
}

void VST3Plugin::getBankChunkData(void **data, size_t *size) const {
}

void VST3Plugin::readProgramFile(const std::string& path){

}

void VST3Plugin::readProgramData(const char *data, size_t size){

}

void VST3Plugin::writeProgramFile(const std::string& path){

}

void VST3Plugin::writeProgramData(std::string& buffer){

}

void VST3Plugin::readBankFile(const std::string& path){

}

void VST3Plugin::readBankData(const char *data, size_t size){

}

void VST3Plugin::writeBankFile(const std::string& path){

}

void VST3Plugin::writeBankData(std::string& buffer){

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

} // vst
