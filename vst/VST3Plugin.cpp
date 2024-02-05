#include "VST3Plugin.h"

#include "Log.h"
#include "FileUtils.h"
#include "MiscUtils.h"
#include "Sync.h"

#if SMTG_OS_LINUX
  #include "WindowX11.h"
#endif

#include <cstring>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <cmath>
#include <codecvt>
#include <locale>
#include <cassert>
#include <thread>

#ifndef UNLOAD_VST3_MODULES
#define UNLOAD_VST3_MODULES 1
#endif

// assume that VST3 parameter display strings
// are always ASCII (for performance reasons)
#ifndef VST3_PARAM_DISPLAY_ASCII
#define VST3_PARAM_DISPLAY_ASCII 1
#endif

#ifndef DEBUG_VST3_PARAMETERS
#define DEBUG_VST3_PARAMETERS 0
#endif

#ifndef DEBUG_VST3_PARAM_CHANGES
#define DEBUG_VST3_PARAM_CHANGES 0
#endif

DEF_CLASS_IID (FUnknown)
DEF_CLASS_IID (IBStream)
DEF_CLASS_IID (IPlugFrame)
#if SMTG_OS_LINUX
  DEF_CLASS_IID (Linux::IRunLoop)
#endif
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
DEF_CLASS_IID (Vst::IEditController2)
DEF_CLASS_IID (Vst::IParameterFinder)
DEF_CLASS_IID (Vst::IAutomationState)
DEF_CLASS_IID (Vst::IMidiMapping)
DEF_CLASS_IID (Vst::IAudioProcessor)
DEF_CLASS_IID (Vst::IUnitInfo)
DEF_CLASS_IID (Vst::IUnitData)
DEF_CLASS_IID (Vst::IProgramListData)
DEF_CLASS_IID (Vst::IAudioPresentationLatency)
DEF_CLASS_IID (Vst::IKeyswitchController)
DEF_CLASS_IID (Vst::IContextMenuTarget)
DEF_CLASS_IID (Vst::IEditControllerHostEditing)
DEF_CLASS_IID (Vst::IXmlRepresentationController)
DEF_CLASS_IID (Vst::INoteExpressionController)
DEF_CLASS_IID (Vst::ChannelContext::IInfoListener)
DEF_CLASS_IID (Vst::IPrefetchableSupport)
DEF_CLASS_IID (Vst::INoteExpressionPhysicalUIMapping)
DEF_CLASS_IID (Vst::IMidiLearn)

#if VST_VERSION >= VST_3_7_0_VERSION
DEF_CLASS_IID (Vst::IProcessContextRequirements)
DEF_CLASS_IID (Vst::IProgress)
#endif

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

/*///////////////// string conversion //////////////////////////*/

// On Wine std::wstring_convert would throw an exception
// when using wchar_t, although it has the same size as char16_t.
// Maybe because of some template specialization? Dunno...
#if defined(_WIN32) && !defined(__WINE__)
using unichar = wchar_t;
#else
using unichar = char16_t;
#endif

using StringCoverter = std::wstring_convert<std::codecvt_utf8_utf16<unichar>, unichar>;

static StringCoverter& stringConverter(){
#ifdef _WIN32
    static_assert(sizeof(wchar_t) == sizeof(char16_t), "bad size for wchar_t!");
#endif
#ifdef __MINGW32__
    // because of a mingw32 bug, destructors of thread_local STL objects segfault...
    thread_local auto conv = new StringCoverter;
    return *conv;
#else
    thread_local StringCoverter conv;
    return conv;
#endif
}

std::string convertString(const Vst::String128 str){
    try {
        return stringConverter().to_bytes(reinterpret_cast<const unichar *>(str));
    } catch (const std::range_error& e){
        throw Error(Error::SystemError, std::string("convertString() failed: ") + e.what());
    }
}

bool convertString (std::string_view src, Steinberg::Vst::String128 dst){
    if (src.size() >= 128) {
        return false;
    }
    try {
        auto wstr = stringConverter().from_bytes(src.begin(), src.end());
        int n = wstr.size() + 1;
        for (int i = 0; i < n; ++i){
            dst[i] = wstr[i];
        }
        return true;
    } catch (const std::range_error& e){
        LOG_ERROR("convertString() failed: " << + e.what());
        return false;
    }
}

/*//////////////////// plugin registry ////////////////////////*/

// NB: these globals are only ever accessed from the UI thread,
// so we don't need a mutex!
static std::unordered_map<uint32_t, VST3Plugin *> gPluginMap;
static UIThread::Handle gPollFunction = UIThread::invalidHandle;

// Queue for read-only or hidden parameters; some plugins use such parameters
// to communicate with the editor or offload work to the UI thread, so we make
// sure that these are dispatched even when the editor is closed.
struct UIParamChange {
    UIParamChange() : pluginId(0), paramId(0), paramValue(0) {}
    UIParamChange(uint32_t pluginId_, Vst::ParamID paramId_, Vst::ParamValue paramValue_)
        : pluginId(pluginId_), paramId(paramId_), paramValue(paramValue_) {}

    uint32_t pluginId;
    Vst::ParamID paramId;
    Vst::ParamValue paramValue;
};

// NOTE: param changes can be safely pushed from multiple threads
// (in case of multithreaded plugin processing).
static UnboundedMPSCQueue<UIParamChange> gParamChangesToGui;
bool gParamChangesInitialized = false;

// called in VST3Plugin ctor (if it has a window)
uint32_t registerPlugin(VST3Plugin *plugin) {
    assert(UIThread::isCurrentThread());

    // lazily initialization, before adding the poll function!!
    if (!gParamChangesInitialized) {
        LOG_DEBUG("VST3: initialize UI queue");
        gParamChangesToGui.reserve(1024);
        gParamChangesInitialized = true;
    }

    static uint32_t nextID = 0;

    auto id = nextID++;

    if (!gPluginMap.emplace(id, plugin).second) {
        throw Error("plugin already registered!");
    }

    // register poll function, if needed
    if (gPollFunction == UIThread::invalidHandle) {
        LOG_DEBUG("VST3: add poll function");
        gPollFunction = UIThread::addPollFunction([](void *) {
            UIParamChange p;
            while (gParamChangesToGui.pop(p)) {
                auto it = gPluginMap.find(p.pluginId);
                if (it != gPluginMap.end()) {
                    it->second->handleUIParamChange(p.paramId, p.paramValue);
                } else {
                #if DEBUG_VST3_PARAM_CHANGES
                    LOG_DEBUG("ignore UI param change: plugin already freed");
                #endif
                }
            }
        }, nullptr);
    }

    return id;
}

// called in VST3Plugin dtor (if it has a window)
void unregisterPlugin(uint32_t id) {
    assert(UIThread::isCurrentThread());

    auto it = gPluginMap.find(id);
    if (it != gPluginMap.end()) {
        gPluginMap.erase(it);

        // remove poll function, if not needed anymore
        if (gPluginMap.empty()) {
            LOG_DEBUG("VST3: remove poll function");
            assert(gPollFunction != UIThread::invalidHandle);
            UIThread::removePollFunction(gPollFunction);
            gPollFunction = UIThread::invalidHandle;
        }
    } else {
        LOG_ERROR("cannot unregister plugin: not found!");
    }
}

/*/////////////////////// VST3Factory /////////////////////////*/

VST3Factory::VST3Factory(const std::string& path, bool probe)
    : PluginFactory(path)
{
    if (probe){
        doLoad();
    }
}

VST3Factory::~VST3Factory(){
    factory_ = nullptr;
#if UNLOAD_VST3_MODULES
    // This crashes on macOS when called in a global/static object
    // destructor! However, proper library termination is required
    // by certain plugins, e.g. Kontakt 7, Guitar Rig 6, etc.
    // Fortunately, SuperCollider plugins may export an "unload"
    // function where we can clear the plugin dictionary.
    // Hopefully, Pd will offer something similar in the future.
    if (module_ && !module_->exit()){
        // don't throw!
        LOG_ERROR("couldn't exit module");
    }
#endif
    // LOG_DEBUG("VST3Factory: deinitialize " << path_);
}

static Mutex gLoaderLock;

void VST3Factory::doLoad(){
    // TODO: optimize with double checked locking?
    std::lock_guard lock(gLoaderLock);

    if (!module_) {
        std::string modulePath = path_;
    #ifndef __APPLE__
        if (isDirectory(modulePath)){
        #ifdef _WIN32
            modulePath += "/" + std::string(getBundleBinaryPath()) + "/" + fileName(path_);
        #else
            modulePath += "/" + std::string(getBundleBinaryPath()) + "/" + fileBaseName(path_) + ".so";
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
        subPlugins_.clear();
        subPluginMap_.clear();
        for (int i = 0; i < numPlugins; ++i){
            PClassInfo ci;
            if (factory_->getClassInfo(i, &ci) == kResultTrue){
                /// LOG_DEBUG("\t" << ci.name << ", " << ci.category);
                if (!strcmp(ci.category, kVstAudioEffectClass)){
                    subPlugins_.push_back(PluginDesc::SubPlugin { ci.name, i });
                    subPluginMap_[ci.name] = i;
                }
            } else {
                throw Error(Error::ModuleError, "Couldn't get class info!");
            }
        }
        // done
        module_ = std::move(module);
    }
}

std::unique_ptr<IPlugin> VST3Factory::create(const std::string& name, bool editor) const {
    const_cast<VST3Factory *>(this)->doLoad(); // lazy loading

    if (plugins_.empty()){
        throw Error(Error::ModuleError, "Factory doesn't have any plugin(s)");
    }
    // find plugin desc
    auto it = pluginMap_.find(name);
    if (it == pluginMap_.end()){
        throw Error(Error::ModuleError, "Can't find (sub)plugin '" + name + "'");
    }
    auto desc = it->second;
    // find plugin index
    auto it2 = subPluginMap_.find(name);
    if (it2 == subPluginMap_.end()){
        throw Error(Error::ModuleError, "Can't find index for (sub)plugin '" + name + "'");
    }
    auto index = it2->second;

    return std::make_unique<VST3Plugin>(factory_, index, shared_from_this(), desc, editor);
}

PluginDesc::const_ptr VST3Factory::probePlugin(int id) const {
    const_cast<VST3Factory *>(this)->doLoad(); // lazy loading

    if (subPlugins_.empty()){
        throw Error(Error::ModuleError, "Factory doesn't have any plugin(s)");
    }

    // if the module contains a single plugin, we don't have to enumerate subplugins!
    if (id < 0){
        if (subPlugins_.size() > 1){
            // only write list of subplugins
            auto desc = std::make_shared<PluginDesc>(nullptr);
            desc->subPlugins = subPlugins_;
            return desc;
        } else {
            id = subPlugins_[0].id; // grab the first (and only) plugin
        }
    }

    // create (sub)plugin
    auto plugin = std::make_unique<VST3Plugin>(factory_, id, shared_from_this(), nullptr, false);
    auto desc = plugin->getInfo();
#if 1
    // HACK for Kontakt 7: when the plugin is destroyed immediately,
    // the "Web Request Worker" thread segfaults with a null pointer access.
    // As a workaround, we simply sleep for half a second. (100 ms would still
    // occasionally crash in release mode, so let's stay on the safe side!)
    if (startsWith(desc->name, "Kontakt ")) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
#endif
    return desc;
}

/*///////////////////// ParamValueQueue /////////////////////*/

#if USE_MULTI_POINT_AUTOMATION

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
    for (auto it = values_.end(); it != values_.begin(); ){
        --it; // decrement here (instead of inside the loop condition) to avoid MSVC debug assertion
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

#endif

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

VST3Plugin::VST3Plugin(IPtr<IPluginFactory> factory, int which, IFactory::const_ptr f,
                       PluginDesc::const_ptr desc, bool editor)
    : info_(std::move(desc)), factory_(std::move(f))
{
    memset(&context_, 0, sizeof(context_));
    context_.state = Vst::ProcessContext::kContTimeValid | Vst::ProcessContext::kSystemTimeValid
            | Vst::ProcessContext::kProjectTimeMusicValid | Vst::ProcessContext::kBarPositionValid
            | Vst::ProcessContext::kCycleValid | Vst::ProcessContext::kTempoValid
            | Vst::ProcessContext::kTimeSigValid | Vst::ProcessContext::kClockValid
            | Vst::ProcessContext::kSmpteValid;
    context_.sampleRate = 44100;
    context_.tempo = 120;
    context_.timeSigNumerator = 4;
    context_.timeSigDenominator = 4;
    context_.frameRate.framesPerSecond = 60; // just pick one

    // are we probing?
    auto newInfo = !info_ ? std::make_shared<PluginDesc>(factory_) : nullptr;
    TUID uid;
    PClassInfo2 ci2;
    auto factory2 = FUnknownPtr<IPluginFactory2>(factory);
    if (factory2 && factory2->getClassInfo2(which, &ci2) == kResultTrue){
        memcpy(uid, ci2.cid, sizeof(TUID));
        if (newInfo){
            newInfo->name = ci2.name;
            newInfo->category = ci2.subCategories;
            newInfo->vendor = ci2.vendor;
            newInfo->version = ci2.version;
            newInfo->sdkVersion = ci2.sdkVersion;
        }
    } else {
        Steinberg::PClassInfo ci;
        if (factory->getClassInfo(which, &ci) == kResultTrue){
            memcpy(uid, ci.cid, sizeof(TUID));
            if (newInfo){
                newInfo->name = ci.name;
                newInfo->category = "Uncategorized";
                newInfo->version = "0.0.0";
                newInfo->sdkVersion = "VST 3";
            }
        } else {
            throw Error(Error::PluginError, "Couldn't get class info!");
        }
    }
    // create component
    LOG_DEBUG("VST3Plugin: create component");
    if (!(component_ = createInstance<Vst::IComponent>(factory, uid))){
        throw Error(Error::PluginError, "Couldn't create VST3 component");
    }
    // initialize component
    LOG_DEBUG("VST3Plugin: initialize component");
    if (component_->initialize(getHostContext()) != kResultOk){
        throw Error(Error::PluginError, "Couldn't initialize VST3 component");
    }
    // first try to get controller from the component part (simple plugins)
    auto controller = FUnknownPtr<Vst::IEditController>(component_);
    if (controller){
        LOG_DEBUG("VST3Plugin: get controller from component");
        controller_ = shared(controller.getInterface());
    } else {
        // if this fails, try to instantiate controller class and initialize it
        LOG_DEBUG("VST3Plugin: create controller");
        TUID controllerCID;
        if (component_->getControllerClassId(controllerCID) == kResultTrue){
            controller_ = createInstance<Vst::IEditController>(factory, controllerCID);
            if (!controller_) {
                throw Error(Error::PluginError, "Couldn't create VST3 controller");
            }
            LOG_DEBUG("VST3Plugin: initialize controller");
            if (controller_->initialize(getHostContext()) != kResultOk){
                throw Error(Error::PluginError, "Couldn't initialize VST3 controller");
            }
        }
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
        LOG_DEBUG("VST3Plugin: connected component and controller");
    }
    // synchronize state
    MemoryStream stream;
    if (component_->getState(&stream) == kResultTrue){
        stream.rewind();
        if (controller_->setComponentState(&stream) == kResultTrue){
            LOG_DEBUG("VST3Plugin: synchronized state");
        } else {
            LOG_DEBUG("VST3Plugin: didn't synchronize state");
        }
    }
    // check processor
    if (!(processor_ = FUnknownPtr<Vst::IAudioProcessor>(component_))){
        throw Error(Error::PluginError, "Could not get VST3 processor");
    }

    // finally set remaining info
    if (newInfo) {
        newInfo->setUID(uid);
        // vendor name (if still empty)
        if (newInfo->vendor.empty()){
            PFactoryInfo i;
            if (factory->getFactoryInfo(&i) == kResultTrue){
                newInfo->vendor = i.vendor;
            } else {
                newInfo->vendor = "Unknown";
            }
        }
        // get input/output busses
        auto collectBusses = [this](auto dir) {
            std::vector<PluginDesc::Bus> result;
            auto count = component_->getBusCount(Vst::kAudio, dir);
            for (int i = 0; i < count; ++i){
                Vst::BusInfo busInfo;
                if (component_->getBusInfo(Vst::kAudio, dir, i, busInfo) == kResultTrue){
                    PluginDesc::Bus bus;
                    bus.numChannels = busInfo.channelCount;
                    bus.label = convertString(busInfo.name);
                    bus.type = (busInfo.busType == Vst::kAux) ?
                                PluginDesc::Bus::Aux : PluginDesc::Bus::Main;
                    result.push_back(std::move(bus));
                }
            }
            return result;
        };

        newInfo->inputs = collectBusses(Vst::kInput);
        newInfo->outputs = collectBusses(Vst::kOutput);

        auto countMidiChannels = [this](Vst::BusDirection dir) -> int {
            auto count = component_->getBusCount(Vst::kEvent, dir);
            for (int i = 0; i < count; ++i){
                Vst::BusInfo bus;
                if (component_->getBusInfo(Vst::kEvent, dir, i, bus) == kResultTrue){
                    if (bus.busType == Vst::kMain){
                        return bus.channelCount;
                    } else {
                        LOG_DEBUG("got aux MIDI bus!");
                    }
                }
            }
            return 0;
        };
        bool midiInput = countMidiChannels(Vst::kInput);
        bool midiOutput = countMidiChannels(Vst::kOutput);

        bool isSynth = (newInfo->category.find(Vst::PlugType::kInstrument) != std::string::npos);

        uint32_t flags = 0;

        flags |= checkEditor() * PluginDesc::HasEditor;
        flags |= isSynth * PluginDesc::IsSynth;
        flags |= hasPrecision(ProcessPrecision::Single) * PluginDesc::SinglePrecision;
        flags |= hasPrecision(ProcessPrecision::Double) * PluginDesc::DoublePrecision;
        flags |= midiInput * PluginDesc::MidiInput;
        flags |= midiOutput * PluginDesc::MidiOutput;
        flags |= checkEditorResizable() * PluginDesc::EditorResizable;

        newInfo->flags = flags;

        // get parameters
        std::set<Vst::ParamID> params;
        int numParameters = controller_->getParameterCount();
        for (int i = 0; i < numParameters; ++i){
            PluginDesc::Param param;
            Vst::ParameterInfo pi;
            if (controller_->getParameterInfo(i, pi) == kResultTrue){
                param.name = convertString(pi.title);
                param.label = convertString(pi.units);
                param.id = pi.id;
                // some plugins have duplicate parameters... why?
                if (params.count(pi.id)){
                    LOG_DEBUG("skip duplicate parameter '" << param.name
                              << "' with ID " << param.id);
                    continue;
                }
                if (pi.flags & Vst::ParameterInfo::kIsProgramChange){
                    newInfo->programChange = pi.id;
                } else if (pi.flags & Vst::ParameterInfo::kIsBypass){
                    newInfo->bypass = pi.id;
                } else {
                #if DEBUG_VST3_PARAMETERS
                    LOG_DEBUG(param.name << ": id=" << pi.id << ", flags=" << pi.flags);
                #endif
                    // Only show automatable/visible parameters.
                    if (!(pi.flags & Vst::ParameterInfo::kIsReadOnly) &&
                        !(pi.flags & Vst::ParameterInfo::kIsHidden)) {
                        param.automatable = pi.flags & Vst::ParameterInfo::kCanAutomate;
                        // Some JUCE plugins have thousands of MIDI CC parameters,
                        // e.g. "MIDI CC 0|0" etc., so we apply the following hack:
                        if (!startsWith(param.name, "MIDI CC ")) {
                            params.insert(param.id);
                            newInfo->addParameter(std::move(param));
                        } else {
                        #if DEBUG_VST3_PARAMETERS
                            LOG_DEBUG("ignore JUCE MIDI CC parameter '" << param.name << "'");
                        #endif
                        }
                    } else {
                    #if DEBUG_VST3_PARAMETERS
                        LOG_DEBUG("ignore read-only parameter '" << param.name << "'");
                    #endif
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
                            newInfo->programs.push_back(convertString(name));
                        } else {
                            LOG_ERROR("couldn't get program name!");
                            newInfo->programs.push_back("");
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
        info_ = newInfo;
    }
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

    // cache for automatable parameters
    int numAutoParams = getNumParameters();
    if (numAutoParams > 0) {
        paramCache_.reset(new std::atomic<float>[numAutoParams]{}); // !
        int numBins = alignTo(numAutoParams, paramCacheBits) / paramCacheBits;
        paramCacheBins_.reset(new std::atomic<size_t>[numBins]{}); // !
        numParamCacheBins_ = numBins;
    }
    updateParameterCache();

#if 1 && VST_VERSION >= VST_3_7_0_VERSION
    // process context requirements
    FUnknownPtr<Vst::IProcessContextRequirements> contextRequirements(processor_);
    if (contextRequirements){
        auto flags = contextRequirements->getProcessContextRequirements();

        LOG_DEBUG("process context requirements:");

#define PRINT_FLAG(x) if (flags & Vst::IProcessContextRequirements::Flags::x) LOG_DEBUG(#x);
        PRINT_FLAG(kNeedSystemTime)
        PRINT_FLAG(kNeedContinousTimeSamples)
        PRINT_FLAG(kNeedProjectTimeMusic)
        PRINT_FLAG(kNeedBarPositionMusic)
        PRINT_FLAG(kNeedCycleMusic)
        PRINT_FLAG(kNeedSamplesToNextClock)
        PRINT_FLAG(kNeedTempo)
        PRINT_FLAG(kNeedTimeSignature)
        PRINT_FLAG(kNeedChord)
        PRINT_FLAG(kNeedFrameRate)
        PRINT_FLAG(kNeedTransportState)
#undef PRINT_FLAG
    } else {
        LOG_DEBUG("IProcessContextRequirements not supported");
    }
#endif

    // NB: don't call hasEditor() because it would create the plugin view!
    if (editor && info_->editor()) {
        uniqueId_ = registerPlugin(this);
        window_ = IWindow::create(*this);
    }

    LOG_DEBUG("VST3Plugin: initialized");
}

VST3Plugin::~VST3Plugin() {
    if (window_) {
        unregisterPlugin(uniqueId_);
    }
    listener_ = nullptr; // for some buggy plugins
    window_ = nullptr;
    // terminate controller - but only if we had to create it!
    if (!FUnknownPtr<Vst::IEditController>(component_)) {
        LOG_DEBUG("VST3Plugin: terminate controller");
        controller_->terminate();
    }
    // terminate component
    LOG_DEBUG("VST3Plugin: terminate component");
    component_->terminate();
    LOG_DEBUG("VST3Plugin: deinitialized");
}

// IComponentHandler
tresult VST3Plugin::beginEdit(Vst::ParamID id){
    LOG_DEBUG("begin edit");
    return kResultOk;
}

tresult VST3Plugin::performEdit(Vst::ParamID id, Vst::ParamValue value){
    int index = info().getParamIndex(id);
    if (index >= 0 && listener_){
        listener_->parameterAutomated(index, value);
    }
    if (window_) {
        paramChangesFromGui_.emplace(index, id, value);
    } else {
        // Can this even happen?
        LOG_DEBUG("performEdit() called without editor: id = "
                  << id << ", value = " << value);
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
        if (listener_){
            listener_->latencyChanged(processor_->getLatencySamples());
        }
    }

    // update parameter cache and send notification to listeners
    // NOTE: restartComponent might be called before paramCache_ is allocated
    if ((flags & Vst::kParamValuesChanged) && paramCache_) {
        updateParameterCache();

        if (listener_){
            listener_->updateDisplay();
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
    auto msg = dynamic_cast<HostMessage *>(message);
    if (msg){
    #if LOGLEVEL > 2
        msg->print();
    #endif
    } else {
        LOG_DEBUG("Message: " << message->getMessageID());
    }
}

tresult VST3Plugin::notify(Vst::IMessage *message){
#if LOGLEVEL > 2
    printMessage(message);
#endif
    sendMessage(message);
    return kResultTrue;
}

#if VST_VERSION >= VST_3_7_0_VERSION

tresult VST3Plugin::start(ProgressType type, const tchar *description, ID& id) {
    static std::atomic<ID> currentID {0};
    id = currentID.fetch_add(1);

    const char *what;
    if (type == AsyncStateRestoration){
        what = "AsyncStateRestoration";
    } else if (type == UIBackgroundTask) {
        what = "UIBackgroundTask";
    } else {
        what = "unknown task";
    }
    std::string desc;
    if (description){
    #ifdef UNICODE
        desc = stringConverter().to_bytes(reinterpret_cast<const unichar *>(description));
    #else
        desc = description;
    #endif
    } else {
        desc = "no description";
    }
    LOG_DEBUG("start " << what << " (" << desc << "), ID: " << id);
    return kResultOk;
}

tresult VST3Plugin::update(ID id, Vst::ParamValue value) {
    LOG_DEBUG("update task " << id << ": " << value);
    return kResultOk;
}

tresult VST3Plugin::finish(ID id) {
    LOG_DEBUG("finished task " << id);
    return kResultOk;
}

#endif

void VST3Plugin::setupProcessing(double sampleRate, int maxBlockSize,
                                 ProcessPrecision precision, ProcessMode mode){
    LOG_DEBUG("VST3Plugin: setupProcessing (sr: " << sampleRate << ", blocksize: " << maxBlockSize
              << ", precision: " << ((precision == ProcessPrecision::Single) ? "single" : "double")
              << ", mode: " << ((mode == ProcessMode::Offline) ? "offline" : "realtime") << ")");
    if (sampleRate <= 0){
        LOG_ERROR("setupProcessing: sample rate must be greater than 0!");
        sampleRate = 44100;
    }
    if (maxBlockSize <= 0){
        LOG_ERROR("setupProcessing: block size must be greater than 0!");
        maxBlockSize = 64;
    }

    vst3::ProcessSetup setup;
    setup.processMode = (mode == ProcessMode::Offline) ? Vst::kOffline : Vst::kRealtime;
    setup.symbolicSampleSize = (precision == ProcessPrecision::Double) ? Vst::kSample64 : Vst::kSample32;
    setup.maxSamplesPerBlock = maxBlockSize;
    setup.sampleRate = sampleRate;

    processor_->setupProcessing(reinterpret_cast<Vst::ProcessSetup&>(setup));

    // only update if sample rate has changed
    if (sampleRate != context_.sampleRate){
        auto ratio = sampleRate / context_.sampleRate;
        context_.projectTimeSamples = (double)context_.projectTimeSamples * ratio;
        context_.continousTimeSamples = (double)context_.continousTimeSamples * ratio;
        context_.sampleRate = sampleRate;
    }

    mode_ = mode;
}

void VST3Plugin::process(ProcessData& data){
    if (data.precision == ProcessPrecision::Double){
        doProcess<double>(data);
    } else {
        doProcess<float>(data);
    }
}

template<typename T>
void VST3Plugin::doProcess(ProcessData& inData){
    // process data
    vst3::ProcessData data;
    data.processMode = (inData.mode == ProcessMode::Offline) ? Vst::kOffline : Vst::kRealtime;
    data.symbolicSampleSize = std::is_same<T, double>::value ? Vst::kSample64 : Vst::kSample32;
    data.numSamples = inData.numSamples;
    data.processContext = &context_;
    // prepare input
    data.numInputs = inData.numInputs;
    data.inputs = (vst3::AudioBusBuffers *)alloca(sizeof(vst3::AudioBusBuffers) * inData.numInputs);
    for (int i = 0; i < data.numInputs; ++i){
        auto& bus = data.inputs[i];
        bus.silenceFlags = 0;
        bus.numChannels = inData.inputs[i].numChannels;
        bus.channelBuffers32 = (Vst::Sample32 **)inData.inputs[i].channelData32;
    }
    // prepare output
    data.numOutputs = inData.numOutputs;
    data.outputs = (vst3::AudioBusBuffers *)alloca(sizeof(vst3::AudioBusBuffers) * inData.numOutputs);
    for (int i = 0; i < data.numOutputs; ++i){
        auto& bus = data.outputs[i];
        bus.silenceFlags = 0;
        bus.numChannels = inData.outputs[i].numChannels;
        bus.channelBuffers32 = (Vst::Sample32 **)inData.outputs[i].channelData32;
    }

    data.inputEvents = &inputEvents_;
    data.outputEvents = &outputEvents_;

    data.inputParameterChanges = &inputParamChanges_;
    data.outputParameterChanges = &outputParamChanges_;

    // send parameter changes from editor to processor
    ParamChange paramChange;
    while (paramChangesFromGui_.pop(paramChange)){
        int32 index;
        auto queue = inputParamChanges_.addParameterData(paramChange.id, index);
        queue->addPoint(0, paramChange.value, index);
        // update cache, but don't notify! NOTE: check for hidden parameter!
        if (paramChange.index >= 0) {
            setCacheParameter(paramChange.index, paramChange.value, false);
        }
    }

    // check bypass state
    auto bypassState = bypass_;
    bool bypassRamp = (bypass_ != lastBypass_);
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
    }
    if ((bypassState == Bypass::Hard) && hasBypass()){
        // if we request a hard bypass from a plugin which has its own bypass method,
        // we use that instead (by just calling the processing method)
        bypassState = Bypass::Off;
        bypassRamp = false;
    }
    lastBypass_ = bypass_;

    // process
    if (bypassState == Bypass::Off){
        // ordinary processing
        processor_->process(reinterpret_cast<Vst::ProcessData&>(data));
    } else {
        bypassProcess<T>(inData, data, bypassState, bypassRamp);
    }

    // clear input queues
    inputEvents_.clear();
    inputParamChanges_.clear();

    // handle outgoing events
    handleEvents();
    handleOutputParameterChanges();

    // update time info (if playing)
    if (context_.state & Vst::ProcessContext::kPlaying){
        // first advance time
        // NOTE: according to the VST3 SDK hostchecker plugin,
        // we should always advance the continous sample time
        // and system time, even if we're not playing...
        context_.continousTimeSamples += data.numSamples;
        context_.projectTimeSamples += data.numSamples;
        double delta = data.numSamples / context_.sampleRate;
        context_.systemTime += delta * 1e9;
        double beats = delta * context_.tempo / 60.0;
        context_.projectTimeMusic += beats;
        // bar start: simply round project time (in bars) down to bar length
        double beatsPerBar = context_.timeSigNumerator / context_.timeSigDenominator * 4.0;
        context_.barPositionMusic = static_cast<int64_t>(context_.projectTimeMusic / beatsPerBar) * beatsPerBar;
        // SMPTE offset
        double projectTimeSeconds = (double)context_.projectTimeSamples / context_.sampleRate;
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
        assert(context_.tempo > 0);
        // 60.0 / 24.0 = 2.5
        double samplesPerClock = (2.5 / context_.tempo) * context_.sampleRate;
        context_.samplesToNextClock = clockTickFract * samplesPerClock;
    }
}

template<typename T>
void VST3Plugin::bypassProcess(ProcessData& inData, vst3::ProcessData& data,
                               Bypass state, bool ramp)
{
    if (bypassSilent_ && !ramp){
        // simple bypass
        bypass(inData);
        return;
    }

    // make temporary input vector - don't touch the original vector!
    data.inputs = (data.numInputs > 0) ?
                (vst3::AudioBusBuffers *)(alloca(sizeof(vst3::AudioBusBuffers) * data.numInputs))
              : nullptr;
    for (int i = 0; i < data.numInputs; ++i){
        auto nin = inData.inputs[i].numChannels;
        data.inputs[i].channelBuffers32 = nin > 0 ?
                    (float **)alloca(sizeof(T *) * nin) : nullptr;
        data.inputs[i].numChannels = nin;
    }

    // dummy input buffer
    auto dummy = (T *)alloca(sizeof(T) * data.numSamples);
    std::fill(dummy, dummy + data.numSamples, 0); // zero!

    int dir;
    T advance;
    if (ramp){
        dir = bypass_ != Bypass::Off;
        advance = (1.f / data.numSamples) * (1 - 2 * dir);
    }

    // prepare bypassing
    for (int i = 0; i < data.numInputs; ++i){
        auto input = (const T **)data.inputs[i].channelBuffers32;
        auto nin = data.inputs[i].numChannels;

        if (state == Bypass::Soft){
            if (ramp && i < data.numOutputs){
                auto output = (T **)data.outputs[i].channelBuffers32;
                auto nout = data.outputs[i].numChannels;
                // fade input to produce a smooth tail with no click
                for (int j = 0; j < nin; ++j){
                    if (j < nout){
                        // write fade in/fade out to *output buffer* and use it as the plugin input.
                        // this works because VST plugins actually work in "replacing" mode.
                        auto in = (const T *)inData.inputs[i].channelData32[j];
                        auto out = output[j];
                        T mix = dir;
                        for (int k = 0; k < data.numSamples; ++k, mix += advance){
                            out[k] = in[k] * mix;
                        }
                        input[j] = output[j];
                    } else {
                        input[j] = dummy; // silence
                    }
                }
            } else {
                for (int j = 0; j < nin; ++j){
                    input[j] = dummy; // silence
                }
            }
        } else {
            // hard bypass
            if (!ramp){
                // silence (for flushing the effect)
                for (int j = 0; j < nin; ++j){
                    input[j] = dummy;
                }
            }
        }
    }

    if (ramp) {
        // process <-> bypass transition
        processor_->process(reinterpret_cast<Vst::ProcessData&>(data));

        if (state == Bypass::Soft){
            // soft bypass
            for (int i = 0; i < data.numOutputs; ++i){
                auto output = (T **)data.outputs[i].channelBuffers32;
                auto nout = data.outputs[i].numChannels;
                auto nin = i < data.numInputs ? data.inputs[i].numChannels : 0;

                for (int j = 0; j < nout; ++j){
                    T mix = dir;
                    auto out = output[j];
                    if (j < nin){
                        // fade in/out unprocessed (original) input
                        auto in = (const T *)inData.inputs[i].channelData32[j];
                        for (int k = 0; k < data.numSamples; ++k, mix += advance){
                            out[k] += in[k] * (1.f - mix);
                        }
                    } else {
                        // just fade in/out
                        for (int k = 0; k < data.numSamples; ++k, mix += advance){
                            out[k] *= mix;
                        }
                    }
                }
            }
            if (dir){
                LOG_DEBUG("process -> soft bypass");
            } else {
                LOG_DEBUG("soft bypass -> process");
            }
        } else {
            // hard bypass
            for (int i = 0; i < data.numOutputs; ++i){
                auto output = (T **)data.outputs[i].channelBuffers32;
                auto nout = data.outputs[i].numChannels;
                auto nin = i < data.numInputs ? data.inputs[i].numChannels : 0;

                for (int j = 0; j < nout; ++j){
                    T mix = dir;
                    auto out = output[j];
                    if (j < nin){
                       // cross fade between plugin output and unprocessed (original) input
                       auto in = (const T *)inData.inputs[i].channelData32[j];
                       for (int k = 0; k < data.numSamples; ++k, mix += advance){
                           out[k] = out[k] * mix + in[k] * (1.f - mix);
                       }
                   } else {
                       // just fade in/out
                       for (int k = 0; k < data.numSamples; ++k, mix += advance){
                           out[k] *= mix;
                       }
                   }
                }
            }
            if (dir){
                LOG_DEBUG("process -> hard bypass");
            } else {
                LOG_DEBUG("hard bypass -> process");
            }
        }
    } else {
        // continue to process with empty input till all outputs are silent
        processor_->process(reinterpret_cast<Vst::ProcessData&>(data));

        // check for silence (RMS < ca. -80dB)
        auto isBusSilent = [](auto bus, auto nchannels, auto nsamples){
            const T threshold = 0.0001;
            for (int i = 0; i < nchannels; ++i){
                auto buf = bus[i];
                T sum = 0;
                for (int j = 0; j < nsamples; ++j){
                    T f = buf[j];
                    sum += f * f;
                }
                if ((sum / nsamples) > (threshold * threshold)){
                    return false;
                }
            }
            return true;
        };

        bool silent = true;

        for (int i = 0; i < data.numOutputs; ++i){
            auto output = (T **)data.outputs[i].channelBuffers32;
            auto nout = data.outputs[i].numChannels;
            if (!isBusSilent(output, nout, data.numSamples)){
                silent = false;
                break;
            }
        }

        if (silent){
            LOG_DEBUG("plugin output became silent!");
        }
        bypassSilent_ = silent;

        if (state == Bypass::Soft){
            // mix output with unprocessed (original) input
            for (int i = 0; i < data.numInputs && i < data.numOutputs; ++i){
                auto input = (const T **)inData.inputs[i].channelData32;
                auto nin = data.inputs[i].numChannels;
                auto output = (T **)data.outputs[i].channelBuffers32;
                auto nout = data.outputs[i].numChannels;

                for (int j = 0; j < nin && j < nout; ++j){
                    auto in = input[j];
                    auto out = output[j];
                    for (int k = 0; k < data.numSamples; ++k){
                        out[k] += in[k];
                    }
                }
            }
        } else {
            // hard bypass: overwrite output - the processing
            // is only supposed to flush the effect.
            // (we use the original data!)
            bypass(inData);
        }
    }
}

#define norm2midi(x) (static_cast<uint8_t>((x) * 127.f) & 127)

void VST3Plugin::handleEvents(){
    if (listener_){
        for (int i = 0; i < outputEvents_.getEventCount(); ++i){
            Vst::Event event;
            outputEvents_.getEvent(i, event);
            if (event.type == Vst::Event::kDataEvent) {
                if (event.data.type == Vst::DataEvent::kMidiSysEx){
                    SysexEvent e((const char *)event.data.bytes, event.data.size);
                    listener_->sysexEvent(e);
                } else {
                    LOG_DEBUG("got unsupported data event");
                }
            } else {
                MidiEvent e;
                switch (event.type) {
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
                    if (event.midiCCOut.controlNumber < 128) {
                        // CC event
                        e.data[0] = 0xb0 | event.midiCCOut.channel;
                        e.data[1] = event.midiCCOut.controlNumber;
                        e.data[2] = event.midiCCOut.value;
                    } else {
                        // special events
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
                        default:
                            LOG_DEBUG("unsupported LegacyMIDICCOut type: "
                                      << event.midiCCOut.controlNumber);
                            continue; // go to next event
                        }
                    }
                    break;
                default:
                    LOG_DEBUG("got unsupported event type: " << event.type);
                    continue; // go to next event
                }
                listener_->midiEvent(e);
            }
        }
        outputEvents_.clear();
    }
}

void VST3Plugin::handleOutputParameterChanges(){
    if (listener_) {
        bool editor = window_ != nullptr;
        bool needQueue = editor && (mode_ == ProcessMode::Realtime);
        int numParams = outputParamChanges_.getParameterCount();
        for (int i = 0; i < numParams; ++i){
            auto data = outputParamChanges_.getParameterData(i);
            if (data) {
                auto id = data->getParameterId();
                int numPoints = data->getPointCount();
                auto index = info().getParamIndex(id);
                if (index >= 0) {
                    // automatable parameter
                    for (int j = 0; j < numPoints; ++j){
                        int32 offset = 0;
                        Vst::ParamValue value = 0;
                        if (data->getPoint(j, offset, value) == kResultOk) {
                            if (editor) {
                                setCacheParameter(index, value, true); // notify
                            } else {
                                setCacheParameter(index, value, false);
                            #if 1
                                // TODO: is this necessary?
                                controller_->setParamNormalized(id, value);
                            #endif
                            }
                            // for now we ignore the sample offset
                            listener_->parameterAutomated(index, value);
                        }
                    }
                } else {
                    // read-only or hidden parameters (e.g. VU meter)
                    // some plugins use this to dispatch work to the UI thread,
                    // so we have to make sure this also works without the editor
                    // and with offline processing!
                    for (int j = 0; j < numPoints; ++j) {
                        int32 offset = 0;
                        Vst::ParamValue value = 0;
                        if (data->getPoint(j, offset, value) == kResultOk) {
                            if (needQueue) {
                                // FIXME: currently, the FIFO is only drained when the editor is open!
                                // Consider using a global FIFO and poll it regularly from the UI thread.
                                gParamChangesToGui.emplace(uniqueId_, id, value);
                            } else {
                                // NOTE: this is not really realtime-safe; for realtime processing,
                                // people are advised to enable the VST editor instead!
                                controller_->setParamNormalized(id, value);
                            }
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

bool VST3Plugin::hasTail() const {
    return getTailSize() != 0;
}

int VST3Plugin::getTailSize() const {
    return processor_->getTailSamples();
}

bool VST3Plugin::hasBypass() const {
    return info().bypass != PluginDesc::NoParamID;
}

void VST3Plugin::setBypass(Bypass state){
    auto bypassID = info().bypass;
    bool haveBypass = bypassID != PluginDesc::NoParamID;
    if (state != bypass_){
        if (state == Bypass::Off){
            // turn bypass off
            if (haveBypass && (bypass_ == Bypass::Hard)){
                doSetParameter(bypassID, 0);
                LOG_DEBUG("plugin bypass off");
            }
            // soft bypass is handled by us
        } else if (bypass_ == Bypass::Off){
            // turn bypass on
            if (haveBypass && (state == Bypass::Hard)){
                doSetParameter(bypassID, 1);
                LOG_DEBUG("plugin bypass on");
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
    return ((uint64_t)1 << n) - 1; // don't mess up the brackets!
}

void VST3Plugin::setNumSpeakers(int *input, int numInputs, int *output, int numOutputs){
    // bus count must match!
    assert(numInputs == info().numInputs());
    assert(numOutputs == info().numOutputs());

    LOG_DEBUG("requested bus arrangement:");
    for (int i = 0; i < numInputs; ++i){
        LOG_DEBUG("input bus " << i << ": " << input[i] << "ch");
    }
    for (int i = 0; i < numOutputs; ++i){
        LOG_DEBUG("output bus " << i << ": " << output[i] << "ch");
    }

    // input speakers
    auto inputSpeakers = numInputs > 0 ?
                (Vst::SpeakerArrangement *)alloca(sizeof(Vst::SpeakerArrangement) * numInputs)
              : nullptr;
    for (int i = 0; i < numInputs; ++i){
        inputSpeakers[i] = makeChannels(input[i]);
    };
    // output speakers
    auto outputSpeakers = numOutputs > 0 ?
                (Vst::SpeakerArrangement *)alloca(sizeof(Vst::SpeakerArrangement) * numOutputs)
              : nullptr;
    for (int i = 0; i < numOutputs; ++i){
        outputSpeakers[i] = makeChannels(output[i]);
    };

    if (processor_->setBusArrangements(inputSpeakers, numInputs,
                                       outputSpeakers, numOutputs) == kResultFalse) {
        LOG_DEBUG("bus arrangement not supported!");
    }

    // we have to (de)activate busses *after* setting the bus arrangement.
    // we also retrieve and save the actual bus channel counts.
    // NOTE: if the users passes 0 channels, this effectively deactivates the bus;
    // however, we still have to check the actual speaker arrangement, so that we
    // pass the correct number of channels in the processing routine, even if the
    // bus is deactivated (see the VST3 docs). Some plugins reject a speaker count
    // of 0, but still allow to deactivate the bus.
    auto checkSpeakers = [this](Vst::BusDirection dir, int *busses, int count){
        for (int i = 0; i < count; ++i){
            bool active = busses[i] > 0;
            // check actual channel count
            Vst::SpeakerArrangement arr;
            if (processor_->getBusArrangement(dir, i, arr) == kResultOk){
                busses[i] = Vst::SpeakerArr::getChannelCount(arr);
            } else {
                // ?
                LOG_WARNING("setNumSpeakers: getBusArrangement not supported");
            }
            // only activate bus if number of speakers is greater than zero
            component_->activateBus(Vst::kAudio, dir, i, active);
        }
    };

    checkSpeakers(Vst::kInput, input, numInputs);
    checkSpeakers(Vst::kOutput, output, numOutputs);

    LOG_DEBUG("actual bus arrangement:");
    for (int i = 0; i < numInputs; ++i){
        LOG_DEBUG("input bus " << i << ": " << input[i] << "ch");
    }
    for (int i = 0; i < numOutputs; ++i){
        LOG_DEBUG("output bus " << i << ": " << output[i] << "ch");
    }
}

int VST3Plugin::getLatencySamples() {
    return processor_->getLatencySamples();
}

void VST3Plugin::setTempoBPM(double tempo){
    if (tempo > 0){
        LOG_DEBUG("setTempoBPM: " << tempo);
        context_.tempo = tempo;
    } else {
        LOG_ERROR("setTempoBPM: tempo must be greater than 0!");
    }
}

void VST3Plugin::setTimeSignature(int numerator, int denominator){
    if (numerator > 0 && denominator > 0){
        LOG_DEBUG("setTimeSignature: " << numerator << "/" << denominator);
        context_.timeSigNumerator = numerator;
        context_.timeSigDenominator = denominator;
    } else {
        LOG_ERROR("setTimeSignature: bad time signature "
                  << numerator << "/" << denominator);
    }
}

void VST3Plugin::setTransportPlaying(bool play){
    LOG_DEBUG("setTransportPlaying: " << play);
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

void VST3Plugin::setTransportAutomationWriting(bool writing) {
    auto oldstate = automationState_.load(std::memory_order_relaxed);
    uint32_t newstate;
    do {
        if (writing) {
            newstate = oldstate | Vst::IAutomationState::kWriteState;
        } else {
            newstate = oldstate & ~Vst::IAutomationState::kWriteState;
        }
        // if no editor, set immediately
        if (!window_) {
            FUnknownPtr<Vst::IAutomationState> automationState(controller_);
            if (automationState){
                automationState->setAutomationState(newstate);
            }
            return;
        }
        // otherwise use CAS to notify UI thread
        newstate |= kAutomationStateChanged;
    } while (!automationState_.compare_exchange_weak(oldstate, newstate));
}

void VST3Plugin::setTransportAutomationReading(bool reading) {
    auto oldstate = automationState_.load(std::memory_order_relaxed);
    uint32_t newstate;
    do {
        if (reading) {
            newstate = oldstate | Vst::IAutomationState::kReadState;
        } else {
            newstate = oldstate & ~Vst::IAutomationState::kReadState;
        }
        // if no editor, set immediately
        if (!window_) {
            FUnknownPtr<Vst::IAutomationState> automationState(controller_);
            if (automationState){
                automationState->setAutomationState(newstate);
            }
            return;
        }
        // otherwise use CAS to notify UI thread
        newstate |= kAutomationStateChanged;
    } while (!automationState_.compare_exchange_weak(oldstate, newstate));
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
    LOG_DEBUG("setTransportPosition: " << beat);
    context_.projectTimeMusic = std::max(beat, 0.0);
    // update project time in samples (assuming the tempo is valid for the whole "project")
    double time = beat / context_.tempo * 60.0;
    context_.projectTimeSamples = time * context_.sampleRate;
}

double VST3Plugin::getTransportPosition() const {
    return context_.projectTimeMusic;
}

void VST3Plugin::sendMidiEvent(const MidiEvent &event){
    Vst::Event e;
    e.busIndex = 0; // LATER allow to choose event bus
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
                doSetParameter(id, data2 / 127.f, event.delta); // don't use plainParamToNormalized()
            } else {
                LOG_WARNING("MIDI CC control number " << data1 << " not supported");
            }
            return;
        }
    case 0xc0: // program change
        {
        #if 1
            doSetProgram(data1);
        #else
            // the following was required by a few plugins,
            // but I think they are just buggy...
            auto id = info().programChange;
            if (id != PluginDesc::NoParamID){
                // don't use plainParamToNormalized()
                doSetParameter(id, data1 / 127.f);
                program_ = data1;
            #if 0
                updateParamCache();
            #endif
            } else {
                LOG_DEBUG("no program change parameter");
            }
        #endif
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

bool VST3Plugin::setParameter(int index, std::string_view str, int sampleOffset){
    Vst::ParamValue value;
    Vst::String128 string;
    auto id = info().getParamID(index);
#if VST3_PARAM_DISPLAY_ASCII
    if (str.size() >= 128) {
        return false;
    }
    for (int i = 0; i < str.size(); ++i) {
        string[i] = str[i];
    }
    string[str.size()] = 0;
#else
    if (!convertString(str, string)) {
        return false;
    }
#endif
    if (controller_->getParamValueByString(id, string, value) == kResultOk){
        doSetParameter(id, value, sampleOffset);
        return true;
    } else {
        return false;
    }
}

void VST3Plugin::doSetParameter(Vst::ParamID id, float value, int32 sampleOffset){
    int32 dummy;
    inputParamChanges_.addParameterData(id, dummy)->addPoint(sampleOffset, value, dummy);
    auto index = info().getParamIndex(id);
    if (index >= 0) {
        // automatable parameter
    #if 1
        // verify parameter value
        value = controller_->normalizedParamToPlain(id, value);
        value = controller_->plainParamToNormalized(id, value);
    #endif
        if (window_) {
            setCacheParameter(index, value, true); // notify
        } else {
            setCacheParameter(index, value, false); // don't notify
            controller_->setParamNormalized(id, value); // is this necessary?
        }
    } else {
        // non-automatable parameter or special parameters (program, bypass)
        if (window_){
            gParamChangesToGui.emplace(uniqueId_, id, value);
        } else {
        #if 1
            // This is required for the program parameter, so that the
            // editor can call restartComponent() with kParamValuesChanged.
            // NOTE: juicysfplugin would actually dead-lock, but that's
            // probably a bug in the plugin...
            controller_->setParamNormalized(id, value);
        #endif
        }
    }
}

float VST3Plugin::getParameter(int index) const {
    return paramCache_[index].load(std::memory_order_relaxed);
}

size_t VST3Plugin::getParameterString(int index, ParamStringBuffer& buffer) const {
    Vst::String128 display;
    Vst::ParamID id = info().getParamID(index);
    float value = getParameter(index);
    if (controller_->getParamStringByValue(id, value, display) == kResultOk) {
    #if VST3_PARAM_DISPLAY_ASCII
        int count = 0;
        while (true) {
            auto c = buffer[count] = display[count];
            if (c != 0) {
                count++;
            } else {
                break;
            }
        }
        return count;
    #else
        auto str = convertString(display);
        assert(str.size() < buffer.size());
        memcpy(buffer.data(), str.data(), str.size() + 1);
        return str.size();
    #endif
    } else {
        buffer[0] = 0;
        return 0;
    }
}

int VST3Plugin::getNumParameters() const {
    return info().numParameters();
}

void VST3Plugin::setCacheParameter(int index, float value, bool notify) {
    paramCache_[index].store(value, std::memory_order_relaxed);
    if (notify) {
        int binIndex = index / paramCacheBits;
        int bitIndex = index & (paramCacheBits - 1);
        auto mask = (size_t)1 << bitIndex;
    #if DEBUG_VST3_PARAM_CHANGES
        // LOG_DEBUG("cache parameter: " << index << " " << binIndex << " " << bitIndex);
    #endif
        paramCacheBins_[binIndex].fetch_or(mask, std::memory_order_release);
    }
}

void VST3Plugin::updateParameterCache(){
    int numParams = getNumParameters();
    for (int i = 0; i < numParams; ++i){
        auto id = info().getParamID(i);
        auto value = controller_->getParamNormalized(id);
        paramCache_[i].store(value, std::memory_order_relaxed);
    }
}

// Create view on demand. This saves memory and reduces load times
// in the frequent case that we create a plugin with UI support,
// but don't actually use the editor. For example, we may only
// need it during development, but not during performances.
void VST3Plugin::createViewLazy(bool nullOk) {
    if (!view_) {
        LOG_DEBUG("VST3Plugin: create view");
        view_ = owned(controller_->createView("editor"));
        assert(nullOk || view_);
    }
}

void VST3Plugin::setProgram(int program){
    if (program >= 0 && program < getNumPrograms()){
        doSetProgram(program);
    } else {
        LOG_WARNING("program number out of range!");
    }
}

void VST3Plugin::doSetProgram(int program){
    auto id = info().programChange;
    if (id != PluginDesc::NoParamID){
        auto value = controller_->plainParamToNormalized(id, program);
        LOG_DEBUG("program change value: " << value);
        doSetParameter(id, value);
        program_ = program;
    #if 0
        // plugin should either send parameter changes
        // or call restartComponent with kParamValuesChanged
        updateParamCache();
    #endif
    } else {
        LOG_DEBUG("no program change parameter");
    }
}

void VST3Plugin::setProgramName(std::string_view name){
    throw Error("not implemented");
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
    StreamView stream(data, size);
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
            fprintf(stderr, "preset: ");
            for (int i = 0; i < 16; ++i) {
                fprintf(stderr, "%02X", (uint8_t)classID[i]);
            }
            fprintf(stderr, "\nplugin: %s\n", info().uniqueID.c_str());
            fflush(stderr);
        #endif
            throw Error("wrong class ID");
        }
    }
    int64 offset = 0;
    stream.readInt64(offset);
    // read chunk list
    stream.setPos(offset);
    checkChunkID(Vst::kChunkList);
    int32 count = 0;
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
        StreamView state(stream.data() + entry.offset, entry.size);
        if (isChunkType(entry.id, Vst::kComponentState)){
            LOG_DEBUG("set component state");
            if (component_->setState(&state) == kResultOk){
                LOG_DEBUG("restored component state");
                // also update controller state!
                state.rewind();
                LOG_DEBUG("set component state for controller");
                if (controller_->setComponentState(&state) == kResultOk){
                    LOG_DEBUG("restored component state for controller");
                } else {
                    LOG_DEBUG("couldn't restore component state for controller");
                }
            } else {
                LOG_WARNING("couldn't restore component state");
            }
        } else if (isChunkType(entry.id, Vst::kControllerState)){
            LOG_DEBUG("set controller state");
            if (controller_->setState(&state) == kResultOk){
                LOG_DEBUG("restored controller state");
            } else {
                LOG_WARNING("couldn't restore controller state");
            }
        } else {
            char type[5];
            memcpy(type, entry.id, 4);
            type[4] = '\0';
            LOG_WARNING("unsupported chunk type '" << type << "'");
        }
    }

    updateParameterCache();
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
    MemoryStream stream;
    stream.writeChunkID(Vst::getChunkID(Vst::kHeader)); // header
    stream.writeInt32(Vst::kFormatVersion); // version
    stream.writeTUID(info().getUID()); // class ID
    stream.writeInt64(0); // skip offset
    // write data
    auto writeChunk = [&](auto& component, Vst::ChunkType type, const char *what){
        ChunkListEntry entry;
        memcpy(entry.id, Vst::getChunkID(type), sizeof(Vst::ChunkID));
        stream.tell(&entry.offset);
        LOG_DEBUG("get " << what << " state");
        if (component->getState(&stream) == kResultOk){
            auto pos = stream.getPos();
            entry.size = pos - entry.offset;
            entries.push_back(entry);
            LOG_DEBUG("wrote " << what << " state (" << entry.size << " bytes)");
        } else {
            LOG_DEBUG("couldn't get " << what << " state");
        }
    };
    writeChunk(component_, Vst::kComponentState, "component");
    writeChunk(controller_, Vst::kControllerState, "controller");
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
    stream.release(buffer);
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

bool VST3Plugin::checkEditor() {
    createViewLazy(true); // may be NULL!
    if (view_) {
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

bool VST3Plugin::checkEditorResizable() {
    createViewLazy(true); // may be NULL!
    if (view_) {
        return view_->canResize() == kResultTrue;
    } else {
        return false;
    }
}

tresult VST3Plugin::resizeView(IPlugView *view, ViewRect *newSize){
    LOG_DEBUG("resizeView");
    if (window_){
        window_->resize(newSize->getWidth(), newSize->getHeight());
    }
    return view->onSize(newSize);
}

#if SMTG_OS_LINUX
tresult VST3Plugin::registerEventHandler(Linux::IEventHandler* handler,
                                         Linux::FileDescriptor fd) {
    LOG_DEBUG("VST3Plugin: registerEventHandler (fd: " << fd << ")");
    X11::EventLoop::instance().registerEventHandler(fd,
        [](int fd, void *obj){ static_cast<Linux::IEventHandler *>(obj)->onFDIsSet(fd); }, handler);
    return kResultOk;
}

tresult VST3Plugin::unregisterEventHandler(Linux::IEventHandler* handler) {
    LOG_DEBUG("VST3Plugin: unregisterEventHandler");
    X11::EventLoop::instance().unregisterEventHandler(handler);
    return kResultOk;
}

tresult VST3Plugin::registerTimer(Linux::ITimerHandler* handler,
                                  Linux::TimerInterval milliseconds) {
    LOG_DEBUG("VST3Plugin: registerTimer (" << milliseconds << " ms)");
    auto fn = [](void *obj){
        static_cast<Linux::ITimerHandler *>(obj)->onTimer();
    };
    X11::EventLoop::instance().registerTimer(milliseconds, fn, handler);
    return kResultOk;
}

tresult VST3Plugin::unregisterTimer(Linux::ITimerHandler* handler) {
    LOG_DEBUG("VST3Plugin: unregisterTimer");
    X11::EventLoop::instance().unregisterTimer(handler);
    return kResultOk;
}
#endif

void VST3Plugin::openEditor(void * window){
    if (editorOpen_){
        return;
    }
    createViewLazy();
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
    } else {
        LOG_ERROR("couldn't open VST3 editor");
    }
    editorOpen_ = true;
}

void VST3Plugin::closeEditor(){
    if (!editorOpen_){
        return;
    }
    assert(view_ != nullptr);
    if (view_->removed() == kResultOk) {
        LOG_DEBUG("closed VST3 editor");
    } else {
        LOG_ERROR("couldn't close VST3 editor");
    }
    editorOpen_ = false;
}

bool VST3Plugin::getEditorRect(Rect& rect) const {
    const_cast<VST3Plugin *>(this)->createViewLazy();
    ViewRect r;
    if (view_->getSize(&r) == kResultOk){
        rect.x = r.left;
        rect.y = r.top;
        rect.w = r.right - r.left;
        rect.h = r.bottom - r.top;
        return true;
    } else {
        return false;
    }
}

void VST3Plugin::updateEditor() {
    // automatable parameters
    int numBins = numParamCacheBins_;
    for (int i = 0; i < numBins; ++i) {
        if (paramCacheBins_[i].load(std::memory_order_relaxed) != 0) {
            auto bits = paramCacheBins_[i].exchange(0, std::memory_order_acquire);
            for (int j = 0; j < paramCacheBits; ++j) {
                if (bits & ((size_t)1 << j)) {
                    int index = i * paramCacheBits + j;
                    auto id = info().getParamID(index);
                    auto value = paramCache_[index].load(std::memory_order_relaxed);
                #if DEBUG_VST3_PARAM_CHANGES
                    LOG_DEBUG("update parameter (index: " << index << ", value: " << value << ")");
                #endif
                    controller_->setParamNormalized(id, value);
                }
            }
        }
    }
    // automation state
    auto oldstate = automationState_.load(std::memory_order_relaxed);
    while (oldstate & kAutomationStateChanged) {
        uint32_t newstate = oldstate & ~kAutomationStateChanged; // clear bit
        if (automationState_.compare_exchange_weak(oldstate, newstate)) {
            // CAS succeeded
            FUnknownPtr<Vst::IAutomationState> automationState(controller_);
            if (automationState){
                LOG_DEBUG("update automation state");
                automationState->setAutomationState(newstate);
            }
            break;
        }
        // try again; oldstate has been updated
    }
}

void VST3Plugin::checkEditorSize(int &width, int &height) const {
    const_cast<VST3Plugin *>(this)->createViewLazy();
    ViewRect rect(0, 0, width, height);
    if (view_->checkSizeConstraint(&rect) == kResultOk){
        width = rect.getWidth();
        height = rect.getHeight();
    }
}

void VST3Plugin::resizeEditor(int width, int height) {
    createViewLazy();
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

void VST3Plugin::handleUIParamChange(Vst::ParamID id, Vst::ParamValue value) {
#if DEBUG_VST3_PARAM_CHANGES
    if (id == info().bypass) {
        LOG_DEBUG("update bypass parameter: " << value);
    } else if (id == info().programChange) {
        LOG_DEBUG("update program parameter: " << value);
    } else {
        LOG_DEBUG("update hidden parameter (id: " << id << ", value: " << value << ")");
    }
#endif
    controller_->setParamNormalized(id, value);
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

/*///////////////////// FUID /////////////////////////////*/

struct GUIDStruct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

void FUID::toString(char *buffer, size_t size) {
    int i = 0;
    assert(size >= Vst::kClassIDSize + 1);
#if COM_COMPATIBLE
    GUIDStruct guid;
    memcpy(&guid, uid, sizeof(GUIDStruct));
    sprintf(buffer, "%08X%04X%04X", guid.data1, guid.data2, guid.data3);
    i += 8;
#endif
    for (; i < (int)sizeof(TUID); ++i){
        // we have to cast to uint8_t!
        sprintf(buffer + 2 * i, "%02X", (uint8_t)uid[i]);
    }
}

void FUID::fromString(const char *s, TUID tuid) {
    int i = 0;
    assert(strlen(s) == Vst::kClassIDSize);
#if COM_COMPATIBLE
    GUIDStruct guid;
    sscanf(s, "%08x", &guid.data1);
    sscanf(s+8, "%04hx", &guid.data2);
    sscanf(s+12, "%04hx", &guid.data3);
    memcpy(tuid, &guid, sizeof(TUID) / 2);
    i += 16;
#endif
    for (; i < Vst::kClassIDSize; i += 2){
        uint32_t temp;
        sscanf(s + i, "%02X", &temp);
        tuid[i / 2] = temp;
    }
}

/*///////////////////// BaseStream ///////////////////////*/

#define DEBUG_STREAM 0

#if DEBUG_STREAM
# define LOG_STREAM(x) LOG_DEBUG(x)
#else
# define LOG_STREAM(x)
#endif

tresult BaseStream::read(void* buffer, int32 numBytes, int32* numBytesRead){
    if (cursor_ < 0 || cursor_ > size()){
        LOG_ERROR("BaseStream: cursor out of range!");
        return kInternalError;
    }
    int available = size() - cursor_;
    if (numBytes > available){
        LOG_DEBUG("BaseStream: " << numBytes << " bytes requested, "
                  << available << " bytes available");
        numBytes = available;
    }
    memcpy(buffer, data() + cursor_, numBytes);
    cursor_ += numBytes;
    if (numBytesRead){
        *numBytesRead = numBytes;
    }
    LOG_STREAM("BaseStream: read " << numBytes << " bytes");
    return kResultOk;
}

tresult BaseStream::tell(int64* pos){
    if (pos){
        *pos = cursor_;
        LOG_STREAM("BaseStream: tell");
        return kResultTrue;
    } else {
        return kInvalidArgument;
    }
}

void BaseStream::setPos(int64 pos){
    assert(pos >= 0);
    cursor_ = pos;
}

int64 BaseStream::getPos() const {
    return cursor_;
}

void BaseStream::rewind(){
    cursor_ = 0;
}

tresult BaseStream::doSeek(int64 pos, int32 mode, int64* result, bool resize){
    int64_t newPos = -1;
    switch (mode){
    case kIBSeekSet:
        newPos = pos;
        LOG_STREAM("BaseStream: seek set " << pos);
        break;
    case kIBSeekCur:
        newPos = cursor_ + pos;
        LOG_STREAM("BaseStream: seek cursor " << pos);
        break;
    case kIBSeekEnd:
        newPos = size() + pos;
        LOG_STREAM("BaseStream: seek end " << pos);
        break;
    default:
        return kInvalidArgument;
    }
    if (newPos < 0) {
        LOG_ERROR("BaseStream: seek position "
                  << newPos << " out of range!");
        return kInvalidArgument;
    } else if (newPos > size()){
        if (resize){
            // write() will resize appropriately
        } else {
            LOG_ERROR("BaseStream: seek position "
                      << newPos << " out of range!");
        }
        return kInvalidArgument;
    }
    // don't have to resize here
    if (result){
        *result = newPos;
    }
    cursor_ = newPos;
    return kResultTrue;
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

bool BaseStream::writeTUID(const TUID tuid){
    int32 bytesWritten = 0;
    auto str = FUID(tuid).toString();
    write((void *)str.data(), str.size(), &bytesWritten);
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
        FUID::fromString(buf, tuid);
        return true;
    } else {
        return false;
    }
}

/*///////////////////// StreamView ///////////////////////////*/

StreamView::StreamView(const char *data, size_t size){
    assign(data, size);
}
void StreamView::assign(const char *data, size_t size){
    data_ = data;
    size_ = size;
    cursor_ = 0;
}

tresult StreamView::seek(int64 pos, int32 mode, int64* result){
    return doSeek(pos, mode, result, false);
}

tresult StreamView::write (void* buffer, int32 numBytes, int32* numBytesWritten){
    return kNotImplemented;
}

/*///////////////////// MemoryStream //////////////////////////*/

MemoryStream::MemoryStream(const char *data, size_t size){
    buffer_.assign(data, size);
}

tresult MemoryStream::seek(int64 pos, int32 mode, int64* result){
    return doSeek(pos, mode, result, true);
}

tresult MemoryStream::write (void* buffer, int32 numBytes, int32* numBytesWritten){
    if (cursor_ < 0){
        LOG_ERROR("WriteStream: negative cursor!");
        return kInternalError;
    }
    if (numBytes < 0){
        return kInvalidArgument;
    }
    // NOTE: the cursor might have been set past the current size
    // with seek(), but here we actually resize the buffer.
    int wantSize = cursor_ + numBytes;
    if (wantSize > (int64_t)buffer_.size()){
        buffer_.resize(wantSize);
    }
    memcpy(&buffer_[cursor_], buffer, numBytes);
    cursor_ += numBytes;
    if (numBytesWritten){
        *numBytesWritten = numBytes;
    }
    LOG_STREAM("WriteStream: write " << numBytes << " bytes");
    return kResultTrue;
}

void MemoryStream::release(std::string &dest){
    dest = std::move(buffer_);
    cursor_ = 0;
}

/*///////////////////// HostApplication //////////////////////////*/

Vst::IHostApplication *getHostContext(){
    static auto app = new HostApplication;
    return app;
}

HostApplication::HostApplication() {
// NOTE: unfortunately, we cannot do Vst::##name##::id
#define IID(name, supported) { FUID(name), #name, supported }

    supportedInterfaces_ = {
        // add minimum set
        //---VST 3.0.0--------------------------------
        IID(Vst::IComponent::iid, true),
        IID(Vst::IAudioProcessor::iid, true),
        IID(Vst::IEditController::iid, true),
        IID(Vst::IConnectionPoint::iid, true),

        IID(Vst::IUnitInfo::iid, true),
        IID(Vst::IUnitData::iid, true),
        IID(Vst::IProgramListData::iid, true),

        //---VST 3.0.1--------------------------------
        IID(Vst::IMidiMapping::iid, true),

        //---VST 3.1----------------------------------
        IID(Vst::IEditController2::iid, false),

        //---VST 3.0.2--------------------------------
        IID(Vst::IParameterFinder::iid, false),

        //---VST 3.1----------------------------------
        IID(Vst::IAudioPresentationLatency::iid, false),

        //---VST 3.5----------------------------------
        IID(Vst::IKeyswitchController::iid, false),
        IID(Vst::IContextMenuTarget::iid, false),
        IID(Vst::IEditControllerHostEditing::iid, false),
        IID(Vst::IXmlRepresentationController::iid, false),
        IID(Vst::INoteExpressionController::iid, false),

        //---VST 3.6.5--------------------------------
        IID(Vst::ChannelContext::IInfoListener::iid, false),
        IID(Vst::IPrefetchableSupport::iid, false),
        IID(Vst::IAutomationState::iid, true),

        //---VST 3.6.11--------------------------------
        IID(Vst::INoteExpressionPhysicalUIMapping::iid, false),

        //---VST 3.6.12--------------------------------
        IID(Vst::IMidiLearn::iid, false)
    };

#undef IID
}

HostApplication::~HostApplication() {}

tresult PLUGIN_API HostApplication::getName (Vst::String128 name){
    LOG_DEBUG("HostApplication: getName");
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
    LOG_DEBUG("HostApplication: cannot create instance");
    *obj = nullptr;
    return kResultFalse;
}

tresult PLUGIN_API HostApplication::queryInterface (const char* _iid, void** obj){
    // LOG_DEBUG("host: query interface");
    QUERY_INTERFACE (_iid, obj, FUnknown::iid, IHostApplication)
    QUERY_INTERFACE (_iid, obj, IHostApplication::iid, IHostApplication)
    QUERY_INTERFACE (_iid, obj, IPlugInterfaceSupport::iid, IPlugInterfaceSupport)
    *obj = nullptr;
    return kResultFalse;
}

tresult PLUGIN_API HostApplication::isPlugInterfaceSupported(const TUID _iid)
{
    for (auto& item : supportedInterfaces_) {
        // LATER use structured binding
        FUID fuid;
        const char *name;
        bool supported;
        std::tie(fuid, name, supported) = item;
        if (fuid == _iid) {
            if (supported) {
                LOG_DEBUG("HostApplication: interface " << name << " supported!");
                return kResultTrue;
            } else {
                LOG_DEBUG("HostApplication: interface " << name << " not supported!");
                return kResultFalse;
            }
        }
    }
    LOG_DEBUG("HostApplication: unknown interface (" << FUID(_iid).toString() << ")!");
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

HostAttribute::HostAttribute(HostAttribute&& other) noexcept {
    type = other.type;
    size = other.size;
    v = other.v;
    // leave other empty
    other.size = 0;
    other.v.b = nullptr; // also strings
}

HostAttribute& HostAttribute::operator=(HostAttribute&& other) noexcept {
    if (size > 0){
        delete[] v.b; // also strings
    }
    type = other.type;
    size = other.size;
    v = other.v;
    // leave other empty
    other.size = 0;
    other.v.b = nullptr; // also strings
    return *this;
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
    for (auto& [id, attr] : list_) {
        Log log;
        log << id << ": ";
        switch (attr.type){
        case HostAttribute::kInteger:
            log << attr.v.i;
            break;
        case HostAttribute::kFloat:
            log << attr.v.f;
            break;
        case HostAttribute::kString:
            log << convertString(attr.v.s);
            break;
        case HostAttribute::kBinary:
            log << ": [binary]";
            break;
        default:
            log << ": ?";
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
#if 0 && LOGLEVEL > 2
    attributes_->print();
#endif
    return attributes_.get();
}

void HostMessage::print(){
    Log log;
    log << "Message: " << messageID_;
    if (attributes_){
        attributes_->print();
    }
}

} // vst
