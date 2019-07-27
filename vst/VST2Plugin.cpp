#include "Interface.h"
#include "VST2Plugin.h"
#include "Utility.h"

#include <fstream>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

/*------------------ endianess -------------------*/
    // endianess check taken from Pure Data (d_osc.c)
#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__FreeBSD_kernel__) \
    || defined(__OpenBSD__)
#include <machine/endian.h>
#endif

#if defined(__linux__) || defined(__CYGWIN__) || defined(__GNU__) || \
    defined(ANDROID)
#include <endian.h>
#endif

#ifdef __MINGW32__
#include <sys/param.h>
#endif

#ifdef _MSC_VER
/* _MSVC lacks BYTE_ORDER and LITTLE_ENDIAN */
#define LITTLE_ENDIAN 0x0001
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#if !defined(BYTE_ORDER) || !defined(LITTLE_ENDIAN)
#error No byte order defined
#endif

namespace vst {

union union32 {
    float un_float;
    int32_t un_int32;
    char un_bytes[4];
};

// big endian (de)serialization routines (.FXP and .FXB files store all their data in big endian)
static void int32_to_bytes(int32_t i, char *bytes){
    union32 u;
    u.un_int32 = i;
#if BYTE_ORDER == LITTLE_ENDIAN
        // swap endianess
    bytes[0] = u.un_bytes[3];
    bytes[1] = u.un_bytes[2];
    bytes[2] = u.un_bytes[1];
    bytes[3] = u.un_bytes[0];
#else
    bytes[0] = u.un_bytes[0];
    bytes[1] = u.un_bytes[1];
    bytes[2] = u.un_bytes[1];
    bytes[3] = u.un_bytes[3];
#endif
}

static void float_to_bytes(float f, char *bytes){
    union32 u;
    u.un_float = f;
#if BYTE_ORDER == LITTLE_ENDIAN
        // swap endianess
    bytes[0] = u.un_bytes[3];
    bytes[1] = u.un_bytes[2];
    bytes[2] = u.un_bytes[1];
    bytes[3] = u.un_bytes[0];
#else
    bytes[0] = u.un_bytes[0];
    bytes[1] = u.un_bytes[1];
    bytes[2] = u.un_bytes[1];
    bytes[3] = u.un_bytes[3];
#endif
}

static int32_t bytes_to_int32(const char *bytes){
    union32 u;
#if BYTE_ORDER == LITTLE_ENDIAN
        // swap endianess
    u.un_bytes[3] = bytes[0];
    u.un_bytes[2] = bytes[1];
    u.un_bytes[1] = bytes[2];
    u.un_bytes[0] = bytes[3];
#else
    u.un_bytes[0] = bytes[0];
    u.un_bytes[1] = bytes[1];
    u.un_bytes[2] = bytes[2];
    u.un_bytes[3] = bytes[3];
#endif
    return u.un_int32;
}

static float bytes_to_float(const char *bytes){
    union32 u;
#if BYTE_ORDER == LITTLE_ENDIAN
        // swap endianess
    u.un_bytes[3] = bytes[0];
    u.un_bytes[2] = bytes[1];
    u.un_bytes[1] = bytes[2];
    u.un_bytes[0] = bytes[3];
#else
    u.un_bytes[0] = bytes[0];
    u.un_bytes[1] = bytes[1];
    u.un_bytes[2] = bytes[2];
    u.un_bytes[3] = bytes[3];
#endif
    return u.un_float;
}

/*----------- fxProgram and fxBank file structures (see vstfxstore.h)-------------*/
const size_t fxProgramHeaderSize = 56;  // 7 * VstInt32 + 28 character program name
const size_t fxBankHeaderSize = 156;    // 8 * VstInt32 + 124 empty characters
// magic numbers (using CCONST macro from aeffect.h to avoid multicharacter constants)
#define cMagic CCONST('C', 'c', 'n', 'K')
#define fMagic CCONST('F', 'x', 'C', 'k')
#define bankMagic CCONST('F', 'x', 'B', 'k')
#define chunkPresetMagic CCONST('F', 'P', 'C', 'h')
#define chunkBankMagic CCONST('F', 'B', 'C', 'h')

/*/////////////////////// VST2Factory ////////////////////////////*/

VstInt32 VST2Factory::shellPluginID = 0;

VST2Factory::VST2Factory(const std::string& path)
    : path_(path), module_(IModule::load(path))
{
    if (!module_){
        // shouldn't happen...
        throw Error("VST2Factory bug!");
    }
    entry_ = module_->getFnPtr<EntryPoint>("VSTPluginMain");
    if (!entry_){
    #ifdef __APPLE__
        // VST plugins previous to the 2.4 SDK used main_macho for the entry point name
        // kudos to http://teragonaudio.com/article/How-to-make-your-own-VST-host.html
        entry_ = module_->getFnPtr<EntryPoint>("main_macho");
    #else
        entry_ = module_->getFnPtr<EntryPoint>("main");
    #endif
    }
    if (!entry_){
        throw Error("couldn't find entry point (no VST plugin?)");
    }
    // LOG_DEBUG("VST2Factory: loaded " << path);
}

VST2Factory::~VST2Factory(){
    LOG_DEBUG("freed VST2 module " << path_);
}

void VST2Factory::addPlugin(PluginInfo::ptr desc){
    if (!pluginMap_.count(desc->name)){
        plugins_.push_back(desc);
        pluginMap_[desc->name] = desc;
    }
}

PluginInfo::const_ptr VST2Factory::getPlugin(int index) const {
    if (index >= 0 && index < (int)plugins_.size()){
        return plugins_[index];
    } else {
        return nullptr;
    }
}

int VST2Factory::numPlugins() const {
    return plugins_.size();
}

// for testing we don't want to load hundreds of shell plugins
#define SHELL_PLUGIN_LIMIT 1000
// probe subplugins asynchronously with futures or worker threads
#define PROBE_FUTURES 8 // number of futures to wait for
#define PROBE_THREADS 8 // number of worker threads (0: use futures instead of threads)

IFactory::ProbeFuture VST2Factory::probeAsync() {
    plugins_.clear();
    auto f = probePlugin(""); // don't need a name
    /// LOG_DEBUG("got probePlugin future");
    return [this, f=std::move(f)](ProbeCallback callback){
        /// LOG_DEBUG("about to call probePlugin future");
        auto result = f();
        /// LOG_DEBUG("called probePlugin future");
        if (result->shellPlugins_.empty()){
            plugins_ = { result };
            valid_ = result->valid();
            if (callback){
                callback(*result, 0, 1);
            }
        } else {
            // shell plugin!
            int numPlugins = result->shellPlugins_.size();
        #ifdef SHELL_PLUGIN_LIMIT
            numPlugins = std::min<int>(numPlugins, SHELL_PLUGIN_LIMIT);
        #endif
#if !PROBE_THREADS
            /// LOG_DEBUG("numPlugins: " << numPlugins);
            std::vector<std::tuple<int, std::string, PluginInfo::Future>> futures;
            int i = 0;
            while (i < numPlugins){
                futures.clear();
                // probe the next n plugins
                int n = std::min<int>(numPlugins - i, PROBE_FUTURES);
                for (int j = 0; j < n; ++j, ++i){
                    auto& shell = result->shellPlugins_[i];
                    try {
                        /// LOG_DEBUG("probing '" << shell.name << "'");
                        futures.emplace_back(i, shell.name, probePlugin(shell.name, shell.id));
                    } catch (const Error& e){
                        // should we rather propagate the error and break from the loop?
                        LOG_ERROR("couldn't probe '" << shell.name << "': " << e.what());
                    }
                }
                // collect results
                for (auto& tup : futures){
                    int index;
                    std::string name;
                    PluginInfo::Future future;
                    std::tie(index, name, future) = tup;
                    try {
                        auto plugin = future(); // wait for process
                        plugins_.push_back(plugin);
                        // factory is valid if contains at least 1 valid plugin
                        if (plugin->valid()){
                            valid_ = true;
                        }
                        if (callback){
                            callback(*plugin, index, numPlugins);
                        }
                    } catch (const Error& e){
                        // should we rather propagate the error and break from the loop?
                        LOG_ERROR("couldn't probe '" << name << "': " << e.what());
                    }
                }
            }
        }
#else
            /// LOG_DEBUG("numPlugins: " << numPlugins);
            auto& shellPlugins = result->shellPlugins_;
            auto next = shellPlugins.begin();
            auto end = next + numPlugins;
            int count = 0;
            std::deque<std::tuple<int, std::string, PluginInfo::ptr, Error>> results;

            std::mutex mutex;
            std::condition_variable cond;
            int numThreads = std::min<int>(numPlugins, PROBE_THREADS);
            std::vector<std::thread> threads;
            // thread function
            auto threadFun = [&](int i){
                std::unique_lock<std::mutex> lock(mutex);
                while (next != end){
                    auto shell = next++;
                    lock.unlock();
                    try {
                        /// LOG_DEBUG("probing '" << shell.name << "'");
                        auto plugin = probePlugin(shell->name, shell->id)();
                        lock.lock();
                        results.emplace_back(count++, shell->name, plugin, Error{});
                        LOG_DEBUG("thread " << i << ": probed " << shell->name);
                    } catch (const Error& e){
                        lock.lock();
                        results.emplace_back(count++, shell->name, nullptr, e);
                    }
                    cond.notify_one();
                }
                LOG_DEBUG("worker thread " << i << " finished");
            };
            // spawn worker threads
            for (int j = 0; j < numThreads; ++j){
                threads.push_back(std::thread(threadFun, j));
            }
            // collect results
            std::unique_lock<std::mutex> lock(mutex);
            while (count < numPlugins){
                LOG_DEBUG("wait...");
                cond.wait(lock);
                while (results.size() > 0){
                    int index;
                    std::string name;
                    PluginInfo::ptr plugin;
                    Error e;
                    std::tie(index, name, plugin, e) = results.front();
                    results.pop_front();
                    lock.unlock();

                    if (plugin){
                        plugins_.push_back(plugin);
                        // factory is valid if contains at least 1 valid plugin
                        if (plugin->valid()){
                            valid_ = true;
                        }
                        if (callback){
                            callback(*plugin, index, numPlugins);
                        }
                    } else {
                        // should we rather propagate the error and break from the loop?
                        LOG_ERROR("couldn't probe '" << name << "': " << e.what());
                    }

                    LOG_DEBUG(index << " from " << numPlugins);
                    lock.lock();
                }
            }
            lock.unlock(); // !
            LOG_DEBUG("exit loop");
            // join worker threads
            for (auto& thread : threads){
                if (thread.joinable()){
                    thread.join();
                }
            }
            LOG_DEBUG("all worker threads joined");
#endif
        }
        for (auto& desc : plugins_){
            pluginMap_[desc->name] = desc;
        }
    };
}

IPlugin::ptr VST2Factory::create(const std::string& name, bool probe) const {
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
        // only for shell plugins:
        // set (global) current plugin ID (used in hostCallback)
        shellPluginID = desc->id;
    } else {
        // when probing, shell plugin ID is passed as string
        try {
            shellPluginID = std::stol(name);
        } catch (...){
            shellPluginID = 0;
        }
    }

    AEffect *plugin = entry_(&VST2Plugin::hostCallback);
    shellPluginID = 0; // just to be sure

    if (!plugin){
        throw Error("couldn't initialize plugin");
    }
    if (plugin->magic != kEffectMagic){
        throw Error("not a valid VST2.x plugin!");
    }
    return std::make_unique<VST2Plugin>(plugin, shared_from_this(), desc);
}


/*/////////////////////// VST2Plugin /////////////////////////////*/

// initial size of VstEvents queue (can grow later as needed)
#define DEFAULT_EVENT_QUEUE_SIZE 64

VST2Plugin::VST2Plugin(AEffect *plugin, IFactory::const_ptr f, PluginInfo::const_ptr desc)
    : plugin_(plugin), factory_(std::move(f)), desc_(std::move(desc))
{
    memset(&timeInfo_, 0, sizeof(timeInfo_));
    timeInfo_.sampleRate = 44100;
    timeInfo_.tempo = 120;
    timeInfo_.timeSigNumerator = 4;
    timeInfo_.timeSigDenominator = 4;
    timeInfo_.flags = kVstNanosValid | kVstPpqPosValid | kVstTempoValid
            | kVstBarsValid | kVstCyclePosValid | kVstTimeSigValid | kVstClockValid
            | kVstTransportChanged;

        // create VstEvents structure holding VstEvent pointers
    vstEvents_ = (VstEvents *)malloc(sizeof(VstEvents) + DEFAULT_EVENT_QUEUE_SIZE * sizeof(VstEvent *));
    memset(vstEvents_, 0, sizeof(VstEvents)); // zeroing class fields is enough
    vstEventBufferSize_ = DEFAULT_EVENT_QUEUE_SIZE;

    plugin_->user = this;
    dispatch(effOpen);
    dispatch(effMainsChanged, 0, 1);
    // are we probing?
    if (!desc_){
        // later we might directly initialize PluginInfo here and get rid of many IPlugin methods
        // (we implicitly call virtual methods within our constructor, which works in our case but is a bit edgy)
        PluginInfo newDesc(factory_, *this);
        if (dispatch(effGetPlugCategory) == kPlugCategShell){
            VstInt32 nextID = 0;
            char name[64] = { 0 };
            while ((nextID = (plugin_->dispatcher)(plugin_, effShellGetNextPlugin, 0, 0, name, 0))){
                LOG_DEBUG("plugin: " << name << ", ID: " << nextID);
                PluginInfo::ShellPlugin shellPlugin { name, nextID };
                newDesc.shellPlugins_.push_back(std::move(shellPlugin));
            }
        }
        desc_ = std::make_shared<PluginInfo>(std::move(newDesc));
    }
}

VST2Plugin::~VST2Plugin(){
    dispatch(effClose);

        // clear sysex events
    for (auto& sysex : sysexQueue_){
        free(sysex.sysexDump);
    }
    free(vstEvents_);
    LOG_DEBUG("destroyed VST2 plugin");
}

std::string VST2Plugin::getPluginName() const {
    if (desc_) return desc_->name;
    char buf[256] = { 0 };
    dispatch(effGetEffectName, 0, 0, buf);
    return buf;
}

std::string VST2Plugin::getPluginVendor() const {
    char buf[256] = { 0 };
    dispatch(effGetVendorString, 0, 0, buf);
    return buf;
}

std::string VST2Plugin::getPluginCategory() const {
    switch (dispatch(effGetPlugCategory)){
    case kPlugCategEffect:
        return "Effect";
    case kPlugCategSynth:
        return "Synth";
    case kPlugCategAnalysis:
        return "Analysis";
    case kPlugCategMastering:
        return "Mastering";
    case kPlugCategSpacializer:
        return "Spacializer";
    case kPlugCategRoomFx:
        return "RoomFx";
    case kPlugSurroundFx:
        return "SurroundFx";
    case kPlugCategRestoration:
        return "Restoration";
    case kPlugCategOfflineProcess:
        return "OfflineProcess";
    case kPlugCategShell:
        return "Shell";
    case kPlugCategGenerator:
        return "Generator";
    default:
        return "Undefined";
    }
}

std::string VST2Plugin::getPluginVersion() const {
    char buf[64] = { 0 };
    auto n = snprintf(buf, 64, "%d", plugin_->version);
    if (n > 0 && n < 64){
        return buf;
    } else {
        return "";
    }
}

std::string VST2Plugin::getSDKVersion() const {
    switch (dispatch(effGetVstVersion)){
    case 2400:
        return "VST 2.4";
    case 2300:
        return "VST 2.3";
    case 2200:
        return "VST 2.2";
    case 2100:
        return "VST 2.1";
    default:
        return "VST 2";
    }
}

int VST2Plugin::getPluginUniqueID() const {
    return plugin_->uniqueID;
}

int VST2Plugin::canDo(const char *what) const {
    // 1: yes, 0: no, -1: don't know
    return dispatch(effCanDo, 0, 0, (char *)what);
}

intptr_t VST2Plugin::vendorSpecific(int index, intptr_t value, void *p, float opt){
    return dispatch(effVendorSpecific, index, value, p, opt);
}

void VST2Plugin::process(const float **inputs,
    float **outputs, VstInt32 sampleFrames){
    preProcess(sampleFrames);
    if (plugin_->processReplacing){
        (plugin_->processReplacing)(plugin_, (float **)inputs, outputs, sampleFrames);
    }
    postProcess(sampleFrames);
}

void VST2Plugin::processDouble(const double **inputs,
    double **outputs, VstInt32 sampleFrames){
    preProcess(sampleFrames);
    if (plugin_->processDoubleReplacing){
        (plugin_->processDoubleReplacing)(plugin_, (double **)inputs, outputs, sampleFrames);
    }
    postProcess(sampleFrames);
}

bool VST2Plugin::hasPrecision(ProcessPrecision precision) const {
    if (precision == ProcessPrecision::Single){
        return plugin_->flags & effFlagsCanReplacing;
    } else {
        return plugin_->flags & effFlagsCanDoubleReplacing;
    }
}

void VST2Plugin::setPrecision(ProcessPrecision precision){
    dispatch(effSetProcessPrecision, 0,
             precision == ProcessPrecision::Single ?  kVstProcessPrecision32 : kVstProcessPrecision64);
}

void VST2Plugin::suspend(){
    dispatch(effMainsChanged, 0, 0);
}

void VST2Plugin::resume(){
    dispatch(effMainsChanged, 0, 1);
}

void VST2Plugin::setSampleRate(float sr){
    if (sr > 0){
        dispatch(effSetSampleRate, 0, 0, NULL, sr);
        if (sr != timeInfo_.sampleRate){
            timeInfo_.sampleRate = sr;
            setTransportPosition(0);
        }
    } else {
        LOG_WARNING("setSampleRate: sample rate must be greater than 0!");
    }
}

void VST2Plugin::setBlockSize(int n){
    dispatch(effSetBlockSize, 0, n);
}

int VST2Plugin::getNumInputs() const {
    return plugin_->numInputs;
}

int VST2Plugin::getNumOutputs() const {
    return plugin_->numOutputs;
}

bool VST2Plugin::isSynth() const {
    return hasFlag(effFlagsIsSynth);
}

bool VST2Plugin::hasTail() const {
    return !hasFlag(effFlagsNoSoundInStop);
}

int VST2Plugin::getTailSize() const {
    return dispatch(effGetTailSize);
}

bool VST2Plugin::hasBypass() const {
    return canDo("bypass") > 0;
}

void VST2Plugin::setBypass(bool bypass){
    dispatch(effSetBypass, 0, bypass);
}

void VST2Plugin::setNumSpeakers(int in, int out){
    in = std::max<int>(0, in);
    out = std::max<int>(0, out);
    // VstSpeakerArrangment already has 8 speakers
    int ain = std::max<int>(0, in - 8);
    int aout = std::max<int>(0, out - 8);
    auto input = (VstSpeakerArrangement *)calloc(sizeof(VstSpeakerArrangement) + sizeof(VstSpeakerProperties) * ain, 1);
    auto output = (VstSpeakerArrangement *)calloc(sizeof(VstSpeakerArrangement) + sizeof(VstSpeakerProperties) * aout, 1);
    input->numChannels = in;
    output->numChannels = out;
    auto setSpeakerArr = [](VstSpeakerArrangement& arr, int num){
        switch (num){
        case 0:
            arr.type = kSpeakerArrEmpty;
            break;
        case 1:
            arr.type = kSpeakerArrMono;
            arr.speakers[0].type = kSpeakerM;
            break;
        case 2:
            arr.type = kSpeakerArrStereo;
            arr.speakers[0].type = kSpeakerL;
            arr.speakers[1].type = kSpeakerR;
            break;
        default:
            arr.type = kSpeakerArrUserDefined;
            for (int i = 0; i < num; ++i){
                arr.speakers[i].type = kSpeakerUndefined;
            }
            break;
        }
    };
    setSpeakerArr(*input, in);
    setSpeakerArr(*output, out);
    dispatch(effSetSpeakerArrangement, 0, reinterpret_cast<VstIntPtr>(input), output);
    free(input);
    free(output);
}

void VST2Plugin::setTempoBPM(double tempo){
    if (tempo > 0) {
        timeInfo_.tempo = tempo;
        timeInfo_.flags |= kVstTransportChanged;
    } else {
        LOG_WARNING("setTempoBPM: tempo must be greater than 0!");
    }
}

void VST2Plugin::setTimeSignature(int numerator, int denominator){
    if (numerator > 0 && denominator > 0){
        timeInfo_.timeSigNumerator = numerator;
        timeInfo_.timeSigDenominator = denominator;
        timeInfo_.flags |= kVstTransportChanged;
    } else {
        LOG_WARNING("setTimeSignature: bad time signature!");
    }
}

void VST2Plugin::setTransportPlaying(bool play){
    if (play != (bool)(timeInfo_.flags & kVstTransportPlaying)){
        LOG_DEBUG("setTransportPlaying: " << play);
        timeInfo_.flags ^= kVstTransportPlaying; // toggle
        timeInfo_.flags |= kVstTransportChanged;
    }
}

void VST2Plugin::setTransportRecording(bool record){
    if (record != (bool)(timeInfo_.flags & kVstTransportRecording)){
        LOG_DEBUG("setTransportRecording: " << record);
        timeInfo_.flags ^= kVstTransportRecording; // toggle
        timeInfo_.flags |= kVstTransportChanged;
    }
}

void VST2Plugin::setTransportAutomationWriting(bool writing){
    if (writing != (bool)(timeInfo_.flags & kVstAutomationWriting)){
        timeInfo_.flags ^= kVstAutomationWriting; // toggle
        timeInfo_.flags |= kVstTransportChanged;
    }
}

void VST2Plugin::setTransportAutomationReading(bool reading){
    if (reading != (bool)(timeInfo_.flags & kVstAutomationReading)){
        timeInfo_.flags ^= kVstAutomationReading; // toggle
        timeInfo_.flags |= kVstTransportChanged;
    }
}

void VST2Plugin::setTransportCycleActive(bool active){
    if (active != (bool)(timeInfo_.flags & kVstTransportCycleActive)){
        LOG_DEBUG("setTransportCycleActive: " << active);
        timeInfo_.flags ^= kVstTransportCycleActive; // toggle
        timeInfo_.flags |= kVstTransportChanged;
    }
}

void VST2Plugin::setTransportCycleStart(double beat){
    timeInfo_.cycleStartPos = std::max(0.0, beat);
    timeInfo_.flags |= kVstTransportChanged;
}

void VST2Plugin::setTransportCycleEnd(double beat){
    timeInfo_.cycleEndPos = std::max(0.0, beat);
    timeInfo_.flags |= kVstTransportChanged;
}

void VST2Plugin::setTransportPosition(double beat){
    timeInfo_.ppqPos = std::max(beat, 0.0); // musical position
    double sec = timeInfo_.ppqPos / timeInfo_.tempo * 60.0;
    timeInfo_.nanoSeconds = sec * 1e-009; // system time in nanoseconds
    timeInfo_.samplePos = sec * timeInfo_.sampleRate; // sample position
    timeInfo_.flags |= kVstTransportChanged;
}

int VST2Plugin::getNumMidiInputChannels() const {
    return dispatch(effGetNumMidiInputChannels);
}

int VST2Plugin::getNumMidiOutputChannels() const {
    return dispatch(effGetNumMidiOutputChannels);
}

bool VST2Plugin::hasMidiInput() const {
    return canDo("receiveVstMidiEvent") > 0;
}

bool VST2Plugin::hasMidiOutput() const {
    return canDo("sendVstMidiEvent") > 0;
}

void VST2Plugin::sendMidiEvent(const MidiEvent &event){
    VstMidiEvent midievent;
    memset(&midievent, 0, sizeof(VstMidiEvent));
    midievent.type = kVstMidiType;
    midievent.byteSize = sizeof(VstMidiEvent);
    midievent.deltaFrames = event.delta;
    memcpy(&midievent.midiData, &event.data, sizeof(event.data));

    midiQueue_.push_back(midievent);

    vstEvents_->numEvents++;
}

void VST2Plugin::sendSysexEvent(const SysexEvent &event){
    VstMidiSysexEvent sysexevent;
    memset(&sysexevent, 0, sizeof(VstMidiSysexEvent));
    sysexevent.type = kVstSysExType;
    sysexevent.byteSize = sizeof(VstMidiSysexEvent);
    sysexevent.deltaFrames = event.delta;
    sysexevent.dumpBytes = event.data.size();
    sysexevent.sysexDump = (char *)malloc(sysexevent.dumpBytes);
        // copy the sysex data (LATER figure out how to avoid this)
    memcpy(sysexevent.sysexDump, event.data.data(), sysexevent.dumpBytes);

    sysexQueue_.push_back(std::move(sysexevent));

    vstEvents_->numEvents++;
}

void VST2Plugin::setParameter(int index, float value){
    plugin_->setParameter(plugin_, index, value);
}

bool VST2Plugin::setParameter(int index, const std::string &str){
    return dispatch(effString2Parameter, index, 0, (void *)str.c_str());
}

float VST2Plugin::getParameter(int index) const {
    return (plugin_->getParameter)(plugin_, index);
}

std::string VST2Plugin::getParameterName(int index) const {
    char buf[256] = {0};
    dispatch(effGetParamName, index, 0, buf);
    return std::string(buf);
}

std::string VST2Plugin::getParameterLabel(int index) const {
    char buf[256] = {0};
    dispatch(effGetParamLabel, index, 0, buf);
    return std::string(buf);
}

std::string VST2Plugin::getParameterDisplay(int index) const {
    char buf[256] = {0};
    dispatch(effGetParamDisplay, index, 0, buf);
    return std::string(buf);
}

int VST2Plugin::getNumParameters() const {
    return plugin_->numParams;
}

void VST2Plugin::setProgram(int program){
    if (program >= 0 && program < getNumPrograms()){
        dispatch(effBeginSetProgram);
        dispatch(effSetProgram, 0, program);
        dispatch(effEndSetProgram);
            // update();
    } else {
        LOG_WARNING("program number out of range!");
    }
}

void VST2Plugin::setProgramName(const std::string& name){
    dispatch(effSetProgramName, 0, 0, (void*)name.c_str());
}

int VST2Plugin::getProgram() const {
    return dispatch(effGetProgram, 0, 0, NULL, 0.f);
}

std::string VST2Plugin::getProgramName() const {
    char buf[256] = {0};
    dispatch(effGetProgramName, 0, 0, buf);
    return std::string(buf);
}

std::string VST2Plugin::getProgramNameIndexed(int index) const {
    char buf[256] = {0};
    dispatch(effGetProgramNameIndexed, index, 0, buf);
    return std::string(buf);
}

int VST2Plugin::getNumPrograms() const {
    return plugin_->numPrograms;
}

bool VST2Plugin::hasChunkData() const {
    return hasFlag(effFlagsProgramChunks);
}

void VST2Plugin::setProgramChunkData(const void *data, size_t size){
    dispatch(effSetChunk, true, size, const_cast<void *>(data));
}

void VST2Plugin::getProgramChunkData(void **data, size_t *size) const {
    *size = dispatch(effGetChunk, true, 0, data);
}

void VST2Plugin::setBankChunkData(const void *data, size_t size){
    dispatch(effSetChunk, false, size, const_cast<void *>(data));
}

void VST2Plugin::getBankChunkData(void **data, size_t *size) const {
    *size = dispatch(effGetChunk, false, 0, data);
}

void VST2Plugin::readProgramFile(const std::string& path){
    std::ifstream file(path, std::ios_base::binary);
    if (!file.is_open()){
        throw Error("couldn't open file " + path);
    }
    file.seekg(0, std::ios_base::end);
    std::string buffer;
    buffer.resize(file.tellg());
    file.seekg(0, std::ios_base::beg);
    file.read(&buffer[0], buffer.size());
    readProgramData(buffer.data(), buffer.size());
}

void VST2Plugin::readProgramData(const char *data, size_t size){
    if (size < fxProgramHeaderSize){  // see vstfxstore.h
        throw Error("fxProgram: bad header size");
    }
    const VstInt32 chunkMagic = bytes_to_int32(data);
    const VstInt32 byteSize = bytes_to_int32(data+4);
        // byteSize excludes 'chunkMagic' and 'byteSize' fields
    const size_t totalSize = byteSize + 8;
    const VstInt32 fxMagic = bytes_to_int32(data+8);
    // const VstInt32 version = bytes_to_int32(data+12);
    // const VstInt32 fxID = bytes_to_int32(data+16);
    // const VstInt32 fxVersion = bytes_to_int32(data+20);
    const VstInt32 numParams = bytes_to_int32(data+24);
    const char *prgName = data+28;
    const char *prgData = data + fxProgramHeaderSize;
    if (chunkMagic != cMagic){
        throw Error("fxProgram: bad format");
    }
    if (totalSize > size){
        throw Error("fxProgram: too little data");
    }

    if (fxMagic == fMagic){ // list of parameters
        if (hasChunkData()){
            throw Error("fxProgram: plugin expects chunk data");
        }
        if (numParams * sizeof(float) > totalSize - fxProgramHeaderSize){
            throw Error("fxProgram: byte size doesn't match number of parameters");
        }
        setProgramName(prgName);
        for (int i = 0; i < numParams; ++i){
            setParameter(i, bytes_to_float(prgData));
            prgData += sizeof(float);
        }
    } else if (fxMagic == chunkPresetMagic){ // chunk data
        if (!hasChunkData()){
            throw Error("fxProgram: plugin doesn't expect chunk data");
        }
        const size_t chunkSize = bytes_to_int32(prgData);
        if (chunkSize != totalSize - fxProgramHeaderSize - 4){
            throw Error("fxProgram: wrong chunk size");
        }
        setProgramName(prgName);
        setProgramChunkData(prgData + 4, chunkSize);
    } else {
        throw Error("fxProgram: bad format");
    }
}

void VST2Plugin::writeProgramFile(const std::string& path){
    std::ofstream file(path, std::ios_base::binary | std::ios_base::trunc);
    if (!file.is_open()){
        throw Error("couldn't create file " + path);
    }
    std::string buffer;
    writeProgramData(buffer);
    file.write(buffer.data(), buffer.size());
}

void VST2Plugin::writeProgramData(std::string& buffer){
    VstInt32 header[7];
    header[0] = cMagic;
    header[3] = 1; // format version (always 1)
    header[4] = getPluginUniqueID();
    header[5] = plugin_->version;
    header[6] = getNumParameters();

    char prgName[28];
    strncpy(prgName, getProgramName().c_str(), 27);
    prgName[27] = '\0';

    if (!hasChunkData()){
            // parameters
        header[2] = fMagic;
        const int nparams = header[6];
        const size_t totalSize = fxProgramHeaderSize + nparams * sizeof(float);
        header[1] = totalSize - 8; // byte size: totalSize - 'chunkMagic' - 'byteSize'
        buffer.resize(totalSize);
        char *bufptr = &buffer[0];
            // serialize header
        for (int i = 0; i < 7; ++i){
            int32_to_bytes(header[i], bufptr);
            bufptr += 4;
        }
            // serialize program name
        memcpy(bufptr, prgName, 28);
        bufptr += 28;
            // serialize parameters
        for (int i = 0; i < nparams; ++i){
            float_to_bytes(getParameter(i), bufptr);
            bufptr += sizeof(float);
        }
    } else {
            // chunk data
        header[2] = chunkPresetMagic;
        char *chunkData = nullptr;
        size_t chunkSize = 0;
        getProgramChunkData((void **)&chunkData, &chunkSize);
        if (!(chunkData && chunkSize)){
                // shouldn't happen...
            throw Error("fxProgram bug: couldn't get chunk data");
        }
            // totalSize: header size + 'size' field + actual chunk data
        const size_t totalSize = fxProgramHeaderSize + 4 + chunkSize;
            // byte size: totalSize - 'chunkMagic' - 'byteSize'
        header[1] = totalSize - 8;
        buffer.resize(totalSize);
        char *bufptr = &buffer[0];
            // serialize header
        for (int i = 0; i < 7; ++i){
            int32_to_bytes(header[i], bufptr);
            bufptr += 4;
        }
            // serialize program name
        memcpy(bufptr, prgName, 28);
        bufptr += 28;
            // serialize chunk data
        int32_to_bytes(chunkSize, bufptr); // size
        memcpy(bufptr + 4, chunkData, chunkSize); // data
    }
}

void VST2Plugin::readBankFile(const std::string& path){
    std::ifstream file(path, std::ios_base::binary);
    if (!file.is_open()){
        throw Error("couldn't open file " + path);
    }
    file.seekg(0, std::ios_base::end);
    std::string buffer;
    buffer.resize(file.tellg());
    file.seekg(0, std::ios_base::beg);
    file.read(&buffer[0], buffer.size());
    readBankData(buffer.data(), buffer.size());
}

void VST2Plugin::readBankData(const char *data, size_t size){
    if (size < fxBankHeaderSize){  // see vstfxstore.h
        throw Error("fxBank: bad header size");
    }
    const VstInt32 chunkMagic = bytes_to_int32(data);
    const VstInt32 byteSize = bytes_to_int32(data+4);
        // byteSize excludes 'chunkMagic' and 'byteSize' fields
    const size_t totalSize = byteSize + 8;
    const VstInt32 fxMagic = bytes_to_int32(data+8);
    // const VstInt32 version = bytes_to_int32(data+12);
    // const VstInt32 fxID = bytes_to_int32(data+16);
    // const VstInt32 fxVersion = bytes_to_int32(data+20);
    const VstInt32 numPrograms = bytes_to_int32(data+24);
    const VstInt32 currentProgram = bytes_to_int32(data + 28);
    const char *bankData = data + fxBankHeaderSize;
    if (chunkMagic != cMagic){
        throw Error("fxBank: bad format");
    }
    if (totalSize > size){
        throw Error("fxBank: too little data");
    }

    if (fxMagic == bankMagic){ // list of parameters
        if (hasChunkData()){
            throw Error("fxBank: plugin expects chunk data");
        }
        const size_t programSize = fxProgramHeaderSize + getNumParameters() * sizeof(float);
        if (numPrograms * programSize > totalSize - fxBankHeaderSize){
            throw Error("fxBank: byte size doesn't match number of programs");
        }
        for (int i = 0; i < numPrograms; ++i){
            setProgram(i);
            readProgramData(bankData, programSize);
            bankData += programSize;
        }
        setProgram(currentProgram);
    } else if (fxMagic == chunkBankMagic){ // chunk data
        if (!hasChunkData()){
            throw Error("fxBank: plugin doesn't expect chunk data");
        }
        const size_t chunkSize = bytes_to_int32(bankData);
        if (chunkSize != totalSize - fxBankHeaderSize - 4){
            throw Error("fxBank: wrong chunk size");
        }
        setBankChunkData(bankData + 4, chunkSize);
    } else {
        throw Error("fxBank: bad format");
    }
}

void VST2Plugin::writeBankFile(const std::string& path){
    std::ofstream file(path, std::ios_base::binary | std::ios_base::trunc);
    if (!file.is_open()){
        throw Error("couldn't create file " + path);
    }
    std::string buffer;
    writeBankData(buffer);
    file.write(buffer.data(), buffer.size());
}

void VST2Plugin::writeBankData(std::string& buffer){
    VstInt32 header[8];
    header[0] = cMagic;
    header[3] = 1; // format version (always 1)
    header[4] = getPluginUniqueID();
    header[5] = plugin_->version;
    header[6] = getNumPrograms();
    header[7] = getProgram();

    if (!hasChunkData()){
            // programs
        header[2] = bankMagic;
        const int nprograms = header[6];
        const size_t programSize = fxProgramHeaderSize + getNumParameters() * sizeof(float);
        const size_t totalSize = fxBankHeaderSize + nprograms * programSize;
        header[1] = totalSize - 8; // byte size: totalSize - 'chunkMagic' - 'byteSize'
        buffer.resize(totalSize);
        char *bufptr = &buffer[0];
            // serialize header
        for (int i = 0; i < 8; ++i){
            int32_to_bytes(header[i], bufptr);
            bufptr += 4;
        }
        bufptr = &buffer[fxBankHeaderSize];
            // serialize programs
        std::string progData; // use intermediate buffer so we can reuse writeProgramData
        for (int i = 0; i < nprograms; ++i){
            setProgram(i);
            writeProgramData(progData);
            if (progData.size() != programSize){
                    // shouldn't happen...
                buffer.clear();
                throw Error("fxBank bug: wrong program data size");
            }
            memcpy(bufptr, progData.data(), progData.size());
            bufptr += programSize;
        }
        setProgram(header[7]); // restore current program
    } else {
            // chunk data
        header[2] = chunkBankMagic;
        char *chunkData = nullptr;
        size_t chunkSize = 0;
        getBankChunkData((void **)&chunkData, &chunkSize);
        if (!(chunkData && chunkSize)){
                // shouldn't happen...
            throw Error("fxBank bug: couldn't get chunk data");
        }
            // totalSize: header size + 'size' field + actual chunk data
        size_t totalSize = fxBankHeaderSize + 4 + chunkSize;
            // byte size: totalSize - 'chunkMagic' - 'byteSize'
        header[1] = totalSize - 8;
        buffer.resize(totalSize);
        char *bufptr = &buffer[0];
            // serialize header
        for (int i = 0; i < 8; ++i){
            int32_to_bytes(header[i], bufptr);
            bufptr += 4;
        }
        bufptr = &buffer[fxBankHeaderSize];
            // serialize chunk data
        int32_to_bytes(chunkSize, bufptr); // size
        memcpy(bufptr + 4, chunkData, chunkSize); // data
    }
}

bool VST2Plugin::hasEditor() const {
    return hasFlag(effFlagsHasEditor);
}

void VST2Plugin::openEditor(void * window){
    dispatch(effEditOpen, 0, 0, window);
}

void VST2Plugin::closeEditor(){
    dispatch(effEditClose);
}

void VST2Plugin::getEditorRect(int &left, int &top, int &right, int &bottom) const {
    ERect* erc = nullptr;
    dispatch(effEditGetRect, 0, 0, &erc);
    if (erc){
        left = erc->left;
        top = erc->top;
        right = erc->right;
        bottom = erc->bottom;
    } else {
        LOG_ERROR("VST2Plugin::getEditorRect: bug!");
    }
}

// private

bool VST2Plugin::hasFlag(VstAEffectFlags flag) const {
    return plugin_->flags & flag;
}

bool VST2Plugin::canHostDo(const char *what) {
    auto matches = [&](const char *s){
        return (bool)(!strcmp(what, s));
    };
    LOG_DEBUG("canHostDo: " << what);
    return matches("sendVstMidiEvent") || matches("receiveVstMidiEvent")
        || matches("sendVstTimeInfo") || matches("receiveVstTimeInfo")
        || matches("sendVstMidiEventFlagIsRealtime")
        || matches("reportConnectionChanges")
        || matches("shellCategory");
}

void VST2Plugin::parameterAutomated(int index, float value){
    if (listener_){
        listener_->parameterAutomated(index, value);
    }
}

VstTimeInfo * VST2Plugin::getTimeInfo(VstInt32 flags){
    double beatsPerBar = (double)timeInfo_.timeSigNumerator / (double)timeInfo_.timeSigDenominator * 4.0;
        // starting position of current bar in beats (e.g. 4.0 for 4.25 in case of 4/4)
    timeInfo_.barStartPos = std::floor(timeInfo_.ppqPos / beatsPerBar) * beatsPerBar;
#if 0
    LOG_DEBUG("request: " << flags);
    if (flags & kVstNanosValid){
        LOG_DEBUG("want system time" << timeInfo_.nanoSeconds * 1e009);
    }
    if (flags & kVstPpqPosValid){
        LOG_DEBUG("want quarter notes " << timeInfo_.ppqPos);
    }
    if (flags & kVstTempoValid){
        LOG_DEBUG("want tempo");
    }
    if (flags & kVstBarsValid){
        LOG_DEBUG("want bar start pos " << timeInfo_.barStartPos);
    }
    if (flags & kVstCyclePosValid){
        LOG_DEBUG("want cycle pos");
    }
    if (flags & kVstTimeSigValid){
        LOG_DEBUG("want time signature");
    }
#endif
    if (flags & kVstSmpteValid){
        if (!vstTimeWarned_){
            LOG_WARNING("SMPTE not supported (yet)!");
            vstTimeWarned_ = true;
        }
    }
    if (flags & kVstClockValid){
            // samples to nearest midi clock
        double clocks = timeInfo_.ppqPos * 24.0;
        double fract = clocks - (long long)clocks;
        if (fract > 0.5){
            fract -= 1.0;
        }
        if (timeInfo_.tempo > 0){
            timeInfo_.samplesToNextClock = fract / 24.0 * 60.0 / timeInfo_.tempo * timeInfo_.sampleRate;
        } else {
            timeInfo_.samplesToNextClock = 0;
        }
        LOG_DEBUG("want MIDI clock");
    }
    return &timeInfo_;
}

void VST2Plugin::preProcess(int nsamples){
        // send MIDI events:
    int numEvents = vstEvents_->numEvents;
        // resize buffer if needed
    while (numEvents > vstEventBufferSize_){
        LOG_DEBUG("vstEvents: grow (numEvents " << numEvents << ", old size " << vstEventBufferSize_<< ")");
            // always grow (doubling the memory), never shrink
        int newSize = vstEventBufferSize_ * 2;
        vstEvents_ = (VstEvents *)realloc(vstEvents_, sizeof(VstEvents) + newSize * sizeof(VstEvent *));
        vstEventBufferSize_ = newSize;
        memset(vstEvents_, 0, sizeof(VstEvents)); // zeroing class fields is enough
        vstEvents_->numEvents = numEvents;
    }
        // set VstEvent pointers (do it right here to ensure they are all valid)
    int n = 0;
    for (auto& midi : midiQueue_){
        vstEvents_->events[n++] = (VstEvent *)&midi;
    }
    for (auto& sysex : sysexQueue_){
        vstEvents_->events[n++] = (VstEvent *)&sysex;
    }
    if (n != numEvents){
        LOG_ERROR("preProcess bug: wrong number of events!");
    } else {
            // always call this, even if there are no events. some plugins depend on this...
        dispatch(effProcessEvents, 0, 0, vstEvents_);
    }
}

void VST2Plugin::postProcess(int nsamples){
        // clear midi events
    midiQueue_.clear();
        // clear sysex events
    for (auto& sysex : sysexQueue_){
        free(sysex.sysexDump);
    }
    sysexQueue_.clear();
        // 'clear' VstEvents array
    vstEvents_->numEvents = 0;

        // advance time (if playing):
    if (timeInfo_.flags & kVstTransportPlaying){
        timeInfo_.samplePos += nsamples; // sample position
        double sec = (double)nsamples / timeInfo_.sampleRate;
        timeInfo_.nanoSeconds += sec * 1e-009; // system time in nanoseconds
        timeInfo_.ppqPos += sec / 60.0 * timeInfo_.tempo;
    }
        // clear flag
    timeInfo_.flags &= ~kVstTransportChanged;
}

void VST2Plugin::processEvents(VstEvents *events){
    int n = events->numEvents;
    for (int i = 0; i < n; ++i){
        auto *event = events->events[i];
        if (event->type == kVstMidiType){
            auto *midiEvent = (VstMidiEvent *)event;
            if (listener_){
                auto *data = midiEvent->midiData;
                listener_->midiEvent(MidiEvent(data[0], data[1], data[2], midiEvent->deltaFrames));
            }
        } else if (event->type == kVstSysExType){
            auto *sysexEvent = (VstMidiSysexEvent *)event;
            if (listener_){
                listener_->sysexEvent(SysexEvent(sysexEvent->sysexDump, sysexEvent->dumpBytes, sysexEvent->deltaFrames));
            }
        } else {
            LOG_VERBOSE("VST2Plugin::processEvents: couldn't process event");
        }
    }
}

VstIntPtr VST2Plugin::dispatch(VstInt32 opCode,
    VstInt32 index, VstIntPtr value, void *p, float opt) const {
    return (plugin_->dispatcher)(plugin_, opCode, index, value, p, opt);
}

// Main host callback

VstIntPtr VSTCALLBACK VST2Plugin::hostCallback(AEffect *plugin, VstInt32 opcode,
    VstInt32 index, VstIntPtr value, void *ptr, float opt){
    switch (opcode){
    case audioMasterCanDo:
        return canHostDo((const char *)ptr);
    case audioMasterVersion:
        // LOG_DEBUG("opcode: audioMasterVersion");
        return 2400;
    case audioMasterCurrentId:
        // LOG_DEBUG("opcode: audioMasterCurrentId");
        // VST2Factory::probeShellPlugins(plugin);
        return VST2Factory::shellPluginID;
    default:
        if (plugin && plugin->user){
            return ((VST2Plugin *)(plugin->user))->callback(opcode, index, value, ptr, opt);
        } else {
            return 0;
        }
    }
}

#define DEBUG_HOSTCODE_IMPLEMENTATION 1
VstIntPtr VST2Plugin::callback(VstInt32 opcode, VstInt32 index, VstIntPtr value, void *p, float opt){
    switch(opcode) {
    case audioMasterAutomate:
        // LOG_DEBUG("opcode: audioMasterAutomate");
        parameterAutomated(index, opt);
        break;
    case audioMasterIdle:
        LOG_DEBUG("opcode: audioMasterIdle");
        dispatch(effEditIdle);
        break;
    case audioMasterGetTime:
        // LOG_DEBUG("opcode: audioMasterGetTime");
        return (VstIntPtr)getTimeInfo(value);
    case audioMasterProcessEvents:
        // LOG_DEBUG("opcode: audioMasterProcessEvents");
        processEvents((VstEvents *)p);
        break;
#if DEBUG_HOSTCODE_IMPLEMENTATION
    case audioMasterIOChanged:
        LOG_DEBUG("opcode: audioMasterIOChanged");
        break;
    case audioMasterSizeWindow:
        LOG_DEBUG("opcode: audioMasterSizeWindow");
        break;
    case audioMasterGetSampleRate:
        LOG_DEBUG("opcode: audioMasterGetSampleRate");
        break;
    case audioMasterGetBlockSize:
        LOG_DEBUG("opcode: audioMasterGetBlockSize");
        break;
    case audioMasterGetInputLatency:
        LOG_DEBUG("opcode: audioMasterGetInputLatency");
        break;
    case audioMasterGetOutputLatency:
        LOG_DEBUG("opcode: audioMasterGetOutputLatency");
        break;
    case audioMasterGetCurrentProcessLevel:
        LOG_DEBUG("opcode: audioMasterGetCurrentProcessLevel");
        break;
    case audioMasterGetAutomationState:
        LOG_DEBUG("opcode: audioMasterGetAutomationState");
        break;
#endif
    case audioMasterGetVendorString:
    case audioMasterGetProductString:
    case audioMasterGetVendorVersion:
    case audioMasterVendorSpecific:
        LOG_DEBUG("opcode: vendor info");
        break;
#if DEBUG_HOSTCODE_IMPLEMENTATION
    case audioMasterGetLanguage:
        LOG_DEBUG("opcode: audioMasterGetLanguage");
        break;
    case audioMasterGetDirectory:
        LOG_DEBUG("opcode: audioMasterGetDirectory");
        break;
    case audioMasterUpdateDisplay:
        LOG_DEBUG("opcode: audioMasterUpdateDisplay");
        break;
    case audioMasterBeginEdit:
        LOG_DEBUG("opcode: audioMasterBeginEdit");
        break;
    case audioMasterEndEdit:
        LOG_DEBUG("opcode: audioMasterEndEdit");
        break;
    case audioMasterOpenFileSelector:
        LOG_DEBUG("opcode: audioMasterOpenFileSelector");
        break;
    case audioMasterCloseFileSelector:
        LOG_DEBUG("opcode: audioMasterCloseFileSelector");
        break;
#endif
    default:
        LOG_DEBUG("plugin requested unknown/deprecated opcode " << opcode);
        return 0;
    }
    return 0; // ?
}

} // vst
