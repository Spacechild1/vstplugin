#include "Interface.h"
#include "VST2Plugin.h"
#include "Utility.h"

#include <fstream>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cassert>

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

VST2Factory::VST2Factory(const std::string& path, bool probe)
    : PluginFactory(path)
{
    if (probe){
        doLoad();
    }
}

VST2Factory::~VST2Factory(){
    // LOG_DEBUG("freed VST2 module " << path_);
}

void VST2Factory::doLoad(){
    if (!module_){
        auto module = IModule::load(path_); // throws on failure
        entry_ = module->getFnPtr<EntryPoint>("VSTPluginMain");
        if (!entry_){
        #ifdef __APPLE__
            // VST plugins previous to the 2.4 SDK used main_macho for the entry point name
            // kudos to http://teragonaudio.com/article/How-to-make-your-own-VST-host.html
            entry_ = module->getFnPtr<EntryPoint>("main_macho");
        #else
            entry_ = module->getFnPtr<EntryPoint>("main");
        #endif
        }
        if (!entry_){
            throw Error(Error::ModuleError, "Couldn't find entry point (not a VST2 plugin?)");
        }
        /// LOG_DEBUG("VST2Factory: loaded " << path_);
        module_ = std::move(module);
    }
}

IPlugin::ptr VST2Factory::create(const std::string &name) const {
    const_cast<VST2Factory *>(this)->doLoad(); // lazy loading

    if (plugins_.empty()){
        throw Error(Error::ModuleError, "Factory doesn't have any plugin(s)");
    }
    auto it = pluginMap_.find(name);
    if (it == pluginMap_.end()){
        throw Error(Error::ModuleError, "can't find (sub)plugin '" + name + "'");
    }
    PluginInfo::ptr desc = it->second;
    // only for shell plugins:
    // set (global) current plugin ID (used in hostCallback)
    shellPluginID = desc->getUniqueID();

    return doCreate(desc);
}

PluginInfo::const_ptr VST2Factory::probePlugin(int id) const {
    const_cast<VST2Factory *>(this)->doLoad(); // lazy loading

    if (id > 0){
        shellPluginID = id;
    } else {
        shellPluginID = 0;
    }

    return doCreate(nullptr)->getInfo();
}

std::unique_ptr<VST2Plugin> VST2Factory::doCreate(PluginInfo::const_ptr desc) const {
    AEffect *plugin = entry_(&VST2Plugin::hostCallback);
    shellPluginID = 0; // just to be sure

    if (!plugin){
        throw Error(Error::PluginError, "couldn't initialize plugin");
    }
    if (plugin->magic != kEffectMagic){
        throw Error(Error::PluginError, "not a valid VST2.x plugin!");
    }
    return std::make_unique<VST2Plugin>(plugin, shared_from_this(), desc);
}


/*/////////////////////// VST2Plugin /////////////////////////////*/

// initial size of VstEvents queue (can grow later as needed)
#define DEFAULT_EVENT_QUEUE_SIZE 64

VST2Plugin::VST2Plugin(AEffect *plugin, IFactory::const_ptr f, PluginInfo::const_ptr desc)
    : plugin_(plugin), info_(std::move(desc))
{
    plugin_->user = this;
    latency_ = plugin->initialDelay;

    memset(&timeInfo_, 0, sizeof(timeInfo_));
    timeInfo_.sampleRate = 44100;
    timeInfo_.tempo = 120;
    timeInfo_.timeSigNumerator = 4;
    timeInfo_.timeSigDenominator = 4;
    timeInfo_.smpteFrameRate = kVstSmpte60fps; // just pick any
    timeInfo_.flags = kVstNanosValid | kVstPpqPosValid | kVstTempoValid
            | kVstBarsValid | kVstCyclePosValid | kVstTimeSigValid
            | kVstClockValid | kVstSmpteValid | kVstTransportChanged;

    // create VstEvents structure holding VstEvent pointers
    vstEvents_ = (VstEvents *)malloc(sizeof(VstEvents) + DEFAULT_EVENT_QUEUE_SIZE * sizeof(VstEvent *));
    memset(vstEvents_, 0, sizeof(VstEvents)); // zeroing class fields is enough
    vstEventBufferSize_ = DEFAULT_EVENT_QUEUE_SIZE;
    // pre-allocate midi queue
    midiQueue_.reserve(DEFAULT_EVENT_QUEUE_SIZE);

    dispatch(effOpen);

    // are we probing?
    if (!info_){
        // create and fill plugin info
        auto info = std::make_shared<PluginInfo>(f);
        info->setUniqueID(plugin_->uniqueID);
        info->name = getPluginName();
        if (info->name.empty()){
            // get from file path
            auto& path = info->path();
            auto sep = path.find_last_of("\\/");
            auto dot = path.find_last_of('.');
            if (sep == std::string::npos){
                sep = -1;
            }
            if (dot == std::string::npos){
                dot = path.size();
            }
            info->name = path.substr(sep + 1, dot - sep - 1);
        }
        info->vendor = getPluginVendor();
        info->category = getPluginCategory();
        info->version = getPluginVersion();
        info->sdkVersion = getSDKVersion();
        PluginInfo::Bus input;
        input.numChannels = getNumInputs();
        info->inputs.emplace_back(std::move(input));
        PluginInfo::Bus output;
        output.numChannels = getNumOutputs();
        info->outputs.emplace_back(std::move(output));
        // flags
        uint32_t flags = 0;
        flags |= hasEditor() * PluginInfo::HasEditor;
        flags |= isSynth() * PluginInfo::IsSynth;
        flags |= hasPrecision(ProcessPrecision::Single) * PluginInfo::SinglePrecision;
        flags |= hasPrecision(ProcessPrecision::Double) * PluginInfo::DoublePrecision;
        flags |= hasMidiInput() * PluginInfo::MidiInput;
        flags |= hasMidiOutput() * PluginInfo::MidiOutput;
        info->flags = flags;
        // get parameters
        int numParameters = getNumParameters();
        for (int i = 0; i < numParameters; ++i){
            PluginInfo::Param p;
            p.name = getParameterName(i);
            p.label = getParameterLabel(i);
            p.id = i;
            info->addParameter(std::move(p));
        }
        // programs
        int numPrograms = getNumPrograms();
        for (int i = 0; i < numPrograms; ++i){
            info->programs.push_back(getProgramNameIndexed(i));
        }
        // VST2 shell plugins only: get sub plugins
        if (dispatch(effGetPlugCategory) == kPlugCategShell){
            LOG_DEBUG("shell plugin");
            VstInt32 nextID = 0;
            char name[256] = { 0 };
            while ((nextID = dispatch(effShellGetNextPlugin, 0, 0, name))){
                LOG_DEBUG("plugin: " << name << ", ID: " << nextID);
                info->subPlugins.push_back(PluginInfo::SubPlugin { name, nextID });
            }
        }
        info_ = info;
    }
    haveBypass_ = hasBypass(); // cache for performance
}

VST2Plugin::~VST2Plugin(){
    listener_.reset(); // for some buggy plugins

    window_ = nullptr;

    dispatch(effClose);

        // clear sysex events
    for (auto& sysex : sysexQueue_){
        free(sysex.sysexDump);
    }
    free(vstEvents_);
    LOG_DEBUG("destroyed VST2 plugin");
}

int VST2Plugin::canDo(const char *what) const {
    // 1: yes, 0: no, -1: don't know
    return dispatch(effCanDo, 0, 0, (char *)what);
}

intptr_t VST2Plugin::vendorSpecific(int index, intptr_t value, void *p, float opt){
    return dispatch(effVendorSpecific, index, value, p, opt);
}

void VST2Plugin::setupProcessing(double sampleRate, int maxBlockSize, ProcessPrecision precision){
    LOG_DEBUG("VST2Plugin: setupProcessing (sr: " << sampleRate << ", blocksize: " << maxBlockSize
              << ", precision: " << ((precision == ProcessPrecision::Single) ? "single" : "double") << ")");
    if (sampleRate > 0){
        dispatch(effSetSampleRate, 0, 0, NULL, sampleRate);
        if (sampleRate != timeInfo_.sampleRate){
            timeInfo_.sampleRate = sampleRate;
            setTransportPosition(0);
        }
    } else {
        LOG_ERROR("setupProcessing: sample rate must be greater than 0!");
    }
    if (maxBlockSize > 0){
        dispatch(effSetBlockSize, 0, maxBlockSize);
    } else {
        LOG_ERROR("setupProcessing: block size be greater than 0!");
    }
    dispatch(effSetProcessPrecision, 0,
             precision == ProcessPrecision::Double ?  kVstProcessPrecision64 : kVstProcessPrecision32);
}

template<typename T, typename TProc>
void VST2Plugin::doProcess(ProcessData& data, TProc processRoutine){
    assert(data.numInputs > 0);
    assert(data.numOutputs > 0);

    if (!processRoutine){
        LOG_ERROR("VST2Plugin::process: no process routine!");
        return; // should never happen!
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
    if ((bypassState == Bypass::Hard) && haveBypass_){
        // if we request a hard bypass from a plugin which has its own bypass method,
        // we use that instead (by just calling the processing method)
        bypassState = Bypass::Off;
        bypassRamp = false;
    }
    lastBypass_ = bypass_;

    // process
    if (bypassState == Bypass::Off){
        // ordinary processing
        processRoutine(plugin_, (T **)data.inputs->channelData32,
                       (T **)data.outputs->channelData32, data.numSamples);
    } else {
        bypassProcess<T>(data, processRoutine, bypassState, bypassRamp);
    }
}

template<typename T, typename TProc>
void VST2Plugin::bypassProcess(ProcessData& data, TProc processRoutine,
                               Bypass state, bool ramp)
{
    if (bypassSilent_ && !ramp){
        // simple bypass
        bypass(data);
        return;
    }

    // make temporary input vector - don't touch the original vector!
    int nin = data.inputs->numChannels;
    auto realInput = (const T **)data.inputs->channelData32;
    auto input = (nin > 0) ? (const T **)alloca(sizeof(T *) * nin) : nullptr;

    int nout = data.outputs->numChannels;
    auto output = (T **)data.outputs->channelData32;

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
    for (int i = 0; i < nin; ++i){
        if (state == Bypass::Soft){
            // fade input to produce a smooth tail with no click
            if (ramp && i < nout){
                // write fade in/fade out to *output buffer* and use it as an input.
                // this works because VST plugins actually work in "replacing" mode.
                auto in = realInput[i];
                auto out = output[i];
                T mix = dir;
                for (int j = 0; j < data.numSamples; ++j, mix += advance){
                    out[j] = in[j] * mix;
                }
                input[i] = output[i];
            } else {
                input[i] = dummy; // silence
            }
        } else {
            // hard bypass
            if (ramp) {
                input[i] = realInput[i]; // for cross-fade
            } else {
                input[i] = dummy; // silence (for flushing the effect)
            }
        }
    }

    if (ramp) {
        // process <-> bypass transition
        processRoutine(plugin_, (T **)input, output, data.numSamples);

        if (state == Bypass::Soft){
            // soft bypass
            for (int i = 0; i < nout; ++i){
                T mix = dir;
                auto out = output[i];
                if (i < nin){
                    // fade in/out unprocessed (original) input
                    auto in = realInput[i];
                    for (int j = 0; j < data.numSamples; ++j, mix += advance){
                        out[j] += in[j] * (1.f - mix);
                    }
                } else {
                    // just fade in/out
                    for (int j = 0; j < data.numSamples; ++j, mix += advance){
                        out[j] *= mix;
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
            for (int i = 0; i < nout; ++i){
               T mix = dir;
               auto out = output[i];
               if (i < nin){
                   // cross fade between plugin output and unprocessed (original) input
                   auto in = realInput[i];
                   for (int j = 0; j < data.numSamples; ++j, mix += advance){
                       out[j] = out[j] * mix + in[j] * (1.f - mix);
                   }
               } else {
                   // just fade in/out
                   for (int j = 0; j < data.numSamples; ++j, mix += advance){
                       out[j] *= mix;
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
        // continue to process with empty input till the output is silent
        processRoutine(plugin_, (T **)input, output, data.numSamples);

        // check for silence (RMS < ca. -80dB)
        auto isSilent = [](auto buf, auto n){
            const T threshold = 0.0001;
            T sum = 0;
            for (int i = 0; i < n; ++i){
                T f = buf[i];
                sum += f * f;
            }
            return (sum / n) < (threshold * threshold); // sqrt(sum/n) < threshold
        };

        bool silent = true;

        for (int i = 0; i < nout; ++i){
            if (!isSilent(output[i], data.numSamples)){
                silent = false;
                break;
            }
        }

        if (silent){
            LOG_DEBUG("plugin output became silent!");
        }
        bypassSilent_ = silent;

        if (state == Bypass::Soft){
            // mix output with unprocessed (cached) input
            for (int i = 0; i < nin && i < nout; ++i){
                auto in = realInput[i];
                auto out = output[i];
                for (int j = 0; j < data.numSamples; ++j){
                    out[j] += in[j];
                }
            }
        } else {
            // hard bypass: overwrite output - the processing
            // is only supposed to flush the effect.
            bypass(data);
        }
    }
}

void VST2Plugin::process(ProcessData& data){
    preProcess(data.numSamples);
    if (data.precision == ProcessPrecision::Double){
        doProcess<double>(data, plugin_->processDoubleReplacing);
    } else {
        doProcess<float>(data, plugin_->processReplacing);
    }
    postProcess(data.numSamples);
}

bool VST2Plugin::hasPrecision(ProcessPrecision precision) const {
    if (precision == ProcessPrecision::Single){
        return plugin_->flags & effFlagsCanReplacing;
    } else {
        return plugin_->flags & effFlagsCanDoubleReplacing;
    }
}

void VST2Plugin::suspend(){
    dispatch(effMainsChanged, 0, 0);
}

void VST2Plugin::resume(){
    dispatch(effMainsChanged, 0, 1);
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

void VST2Plugin::setBypass(Bypass state){
    if (state != bypass_){
        if (state == Bypass::Off){
            // turn bypass off
            if (haveBypass_ && (bypass_ == Bypass::Hard)){
                dispatch(effSetBypass, 0, 0);
                LOG_DEBUG("plugin bypass off");
            }
            // soft bypass is handled by us
        } else if (bypass_ == Bypass::Off){
            // turn bypass on
            if (haveBypass_ && (state == Bypass::Hard)){
                dispatch(effSetBypass, 0, 1);
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

void VST2Plugin::setNumSpeakers(int *input, int numInputs, int *output, int numOutputs){
    assert(numInputs > 0);
    assert(numOutputs > 0);

    LOG_DEBUG("requested speaker arrangement: "
              << *input << " in, " << *output << " out");

    int numInputSpeakers = std::min<int>(*input, getNumInputs());
    int numOutputSpeakers = std::min<int>(*output, getNumOutputs());

    auto initSpeakers = [](VstSpeakerArrangement& arr, int num){
        arr.numChannels = num;
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

    // VstSpeakerArrangment already has 8 speakers
    int inputSpeakerSize = sizeof(VstSpeakerArrangement)
            + sizeof(VstSpeakerProperties) * std::max<int>(0, numInputSpeakers - 8);
    auto inputSpeakers = (VstSpeakerArrangement *)calloc(inputSpeakerSize, 1);
    initSpeakers(*inputSpeakers, numInputSpeakers);

    int outputSpeakerSize = sizeof(VstSpeakerArrangement)
            + sizeof(VstSpeakerProperties) * std::max<int>(0, numOutputSpeakers - 8);
    auto outputSpeakers = (VstSpeakerArrangement *)calloc(outputSpeakerSize, 1);
    initSpeakers(*outputSpeakers, numOutputSpeakers);

    dispatch(effSetSpeakerArrangement, 0,
             reinterpret_cast<VstIntPtr>(inputSpeakers), outputSpeakers);

    free(inputSpeakers);
    free(outputSpeakers);

    // verify speaker arrangement
    inputSpeakers = nullptr;
    outputSpeakers = nullptr;
    dispatch(effGetSpeakerArrangement, 0,
             reinterpret_cast<VstIntPtr>(&inputSpeakers), &outputSpeakers);

    // verify speaker arrangements!
    auto verifySpeakers = [](auto speakerArr, auto defNumSpeakers, auto busses, auto count){
        if (speakerArr){
            *busses = speakerArr->numChannels;
        } else {
            *busses = defNumSpeakers;
        }
        std::fill(busses + 1, busses + count, 0); // zero remaining busses
    };

    verifySpeakers(inputSpeakers, getNumInputs(), input, numInputs);
    verifySpeakers(outputSpeakers, getNumOutputs(), output, numOutputs);

    if (!(inputSpeakers && outputSpeakers)){
        LOG_DEBUG("(effGetSpeakerArrangement not supported)");
    }

    LOG_DEBUG("actual speaker arrangement: "
              << input[0] << " in, " << output[0] << " out");
}

int VST2Plugin::getLatencySamples(){
    return plugin_->initialDelay;
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
        LOG_WARNING("setTimeSignature: bad time signature "
                    << numerator << "/" << denominator << "!");
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
    memcpy(&midievent.midiData, &event.data, sizeof(event.data));
    midievent.deltaFrames = event.delta;
    midievent.detune = event.detune;

    midiQueue_.push_back(midievent);

    vstEvents_->numEvents++;
}

void VST2Plugin::sendSysexEvent(const SysexEvent &event){
    VstMidiSysexEvent sysexevent;
    memset(&sysexevent, 0, sizeof(VstMidiSysexEvent));
    sysexevent.type = kVstSysExType;
    sysexevent.byteSize = sizeof(VstMidiSysexEvent);
    sysexevent.deltaFrames = event.delta;
    sysexevent.dumpBytes = event.size;
    sysexevent.sysexDump = (char *)malloc(sysexevent.dumpBytes);
        // copy the sysex data (LATER figure out how to avoid this)
    memcpy(sysexevent.sysexDump, event.data, sysexevent.dumpBytes);

    sysexQueue_.push_back(std::move(sysexevent));

    vstEvents_->numEvents++;
}

void VST2Plugin::setParameter(int index, float value, int sampleOffset){
    // VST2 can't do sample accurate automation
    plugin_->setParameter(plugin_, index, value);
}

bool VST2Plugin::setParameter(int index, const std::string &str, int sampleOffset){
    // VST2 can't do sample accurate automation
    return dispatch(effString2Parameter, index, 0, (void *)str.c_str());
}

float VST2Plugin::getParameter(int index) const {
    return (plugin_->getParameter)(plugin_, index);
}

std::string VST2Plugin::getParameterString(int index) const {
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
    header[4] = plugin_->uniqueID;
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
    header[4] = plugin_->uniqueID;
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
    if (editor_){
        return;
    }
    dispatch(effEditOpen, 0, 0, window);
    editor_ = true;
}

void VST2Plugin::closeEditor(){
    if (!editor_){
        return;
    }
    dispatch(effEditClose);
    editor_ = false;
}

bool VST2Plugin::getEditorRect(Rect& rect) const {
    ERect* erc = nullptr;
    bool result = dispatch(effEditGetRect, 0, 0, &erc);
    if (erc){
        auto w = erc->right - erc->left;
        auto h = erc->bottom - erc->top;
        // some (buggy) plugins return an empty rect on failure.
        // don't update the input rect!
        if (!(w > 0 && h > 0)){
            return false;
        }
        rect.x = erc->left;
        rect.y = erc->top;
        rect.w = w;
        rect.h = h;
        // some plugins might forget to return '1',
        // other plugins might return a (valid) rect on fail.
        // Either way, we update the coordinates and return false.
        // The return value is only used when we attempt to obtain
        // the window size *before* opening the editor.
        return result;
    }
    return false;
}

void VST2Plugin::updateEditor(){
    dispatch(effEditIdle);
}

void VST2Plugin::checkEditorSize(int &width, int &height) const {}

void VST2Plugin::resizeEditor(int width, int height) {}

bool VST2Plugin::canResize() const { return false; }

// private

std::string VST2Plugin::getPluginName() const {
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
        || matches("shellCategory")
        || matches("supplyIdle")
        || matches("sizeWindow");
}

void VST2Plugin::parameterAutomated(int index, float value){
    auto listener = listener_.lock();
    if (listener){
        listener->parameterAutomated(index, value);
    }
}

#if 0
#define DEBUG_TIME_INFO(x) DO_LOG("plugin wants " << x)
#else
#define DEBUG_TIME_INFO(x)
#endif

VstTimeInfo * VST2Plugin::getTimeInfo(VstInt32 flags){
    if (flags & kVstNanosValid){
        DEBUG_TIME_INFO("system time");
    }
    if (flags & kVstPpqPosValid){
        DEBUG_TIME_INFO("quarter notes");
    }
    if (flags & kVstTempoValid){
        DEBUG_TIME_INFO("tempo");
    }
    if (flags & kVstBarsValid){
        double beatsPerBar = (double)timeInfo_.timeSigNumerator / (double)timeInfo_.timeSigDenominator * 4.0;
            // starting position of current bar in beats (e.g. 4.0 for 4.25 in case of 4/4)
        timeInfo_.barStartPos = std::floor(timeInfo_.ppqPos / beatsPerBar) * beatsPerBar;
        DEBUG_TIME_INFO("bar start pos");
    }
    if (flags & kVstCyclePosValid){
        DEBUG_TIME_INFO("cycle pos");
    }
    if (flags & kVstTimeSigValid){
        DEBUG_TIME_INFO("time signature");
    }
    if (flags & kVstSmpteValid){
        double frames = timeInfo_.samplePos / timeInfo_.sampleRate / 60.0; // our SMPTE frame rate is 60 fps
        double fract = frames - static_cast<int64_t>(frames);
        timeInfo_.smpteOffset = fract * 80; // subframes are 1/80 of a frame
        DEBUG_TIME_INFO("SMPTE offset");
    }
    if (flags & kVstClockValid){
        // samples to nearest midi clock
        double clockTicks = timeInfo_.ppqPos * 24.0;
        double fract = clockTicks - (int64_t)clockTicks;
        // get offset to nearest tick -> can be negative!
        if (fract > 0.5){
            fract -= 1.0;
        }
        if (timeInfo_.tempo > 0){
            double samplesPerClock = (2.5 / timeInfo_.tempo) * timeInfo_.sampleRate; // 60.0 / 24.0 = 2.5
            timeInfo_.samplesToNextClock = fract * samplesPerClock;
        } else {
            timeInfo_.samplesToNextClock = 0;
        }
        DEBUG_TIME_INFO("MIDI clock offset");
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
        double delta = (double)nsamples / timeInfo_.sampleRate;
        timeInfo_.nanoSeconds += delta * 1e-009; // system time in nanoseconds
        timeInfo_.ppqPos += delta * timeInfo_.tempo / 60.0;
    }
    // clear flag
    timeInfo_.flags &= ~kVstTransportChanged;
}

void VST2Plugin::processEvents(VstEvents *events){
    auto listener = listener_.lock();
    if (listener){
        for (int i = 0; i < events->numEvents; ++i){
            auto *event = events->events[i];
            if (event->type == kVstMidiType){
                auto *midiEvent = (VstMidiEvent *)event;
                auto *data = midiEvent->midiData;
                listener->midiEvent(MidiEvent(data[0], data[1], data[2], midiEvent->deltaFrames));
            } else if (event->type == kVstSysExType){
                auto *sysexEvent = (VstMidiSysexEvent *)event;
                listener->sysexEvent(SysexEvent(sysexEvent->sysexDump, sysexEvent->dumpBytes, sysexEvent->deltaFrames));
            } else {
                LOG_VERBOSE("VST2Plugin::processEvents: couldn't process event");
            }
        }
    }
}

VstIntPtr VST2Plugin::dispatch(VstInt32 opCode,
    VstInt32 index, VstIntPtr value, void *p, float opt) const {
    return (plugin_->dispatcher)(plugin_, opCode, index, value, p, opt);
}

// Main host callback
#ifndef DEBUG_HOSTCODE_IMPLEMENTATION
#define DEBUG_HOSTCODE_IMPLEMENTATION 1
#endif

#if DEBUG_HOSTCODE_IMPLEMENTATION
#define DEBUG_HOSTCODE(x) LOG_DEBUG("master opcode: " x)
#else
#define DEBUG_HOSTCODE(x)
#endif

VstIntPtr VSTCALLBACK VST2Plugin::hostCallback(AEffect *plugin, VstInt32 opcode,
    VstInt32 index, VstIntPtr value, void *ptr, float opt){
    switch (opcode){
    case audioMasterCanDo:
        return canHostDo((const char *)ptr);
    case audioMasterVersion:
        DEBUG_HOSTCODE("audioMasterVersion");
        return 2400;
    case audioMasterGetVendorString:
        DEBUG_HOSTCODE("audioMasterGetVendorString");
        strcpy((char *)ptr, "IEM");
        return 1;
    case audioMasterGetProductString:
        DEBUG_HOSTCODE("audioMasterGetProductString");
        strcpy((char *)ptr, "vstplugin");
        return 1;
    case audioMasterGetVendorVersion:
        DEBUG_HOSTCODE("audioMasterGetVendorVersion");
        return 1;
    case audioMasterGetLanguage:
        DEBUG_HOSTCODE("audioMasterGetLanguage");
        return 1;
    case audioMasterCurrentId:
        DEBUG_HOSTCODE("audioMasterCurrentId");
        return VST2Factory::shellPluginID;
    default:
        if (plugin && plugin->user){
            return ((VST2Plugin *)(plugin->user))->callback(opcode, index, value, ptr, opt);
        } else {
        #if DEBUG_HOSTCODE_IMPLEMENTATION
            LOG_DEBUG("requested opcode " << opcode << " before instantiating plugin");
        #endif
            return 0;
        }
    }
}
VstIntPtr VST2Plugin::callback(VstInt32 opcode, VstInt32 index, VstIntPtr value, void *p, float opt){
    switch(opcode) {
    case audioMasterAutomate:
        parameterAutomated(index, opt);
        break;
    case audioMasterIdle:
        DEBUG_HOSTCODE("audioMasterIdle");
        updateEditor();
        break;
    case audioMasterNeedIdle:
        DEBUG_HOSTCODE("audioMasterNeedIdle");
        dispatch(effIdle);
        break;
    case audioMasterWantMidi:
        DEBUG_HOSTCODE("audioMasterWantMidi");
        return 1;
    case audioMasterGetTime:
        return (VstIntPtr)getTimeInfo(value);
    case audioMasterProcessEvents:
        processEvents((VstEvents *)p);
        break;
    case audioMasterIOChanged:
        DEBUG_HOSTCODE("audioMasterIOChanged");
        // check latency
        if (plugin_->initialDelay != latency_){
            auto listener = listener_.lock();
            if (listener){
                listener->latencyChanged(plugin_->initialDelay);
            }
            latency_ = plugin_->initialDelay;
        }
        return 1;
    case audioMasterSizeWindow:
        DEBUG_HOSTCODE("audioMasterSizeWindow");
        if (window_){
            window_->resize(index, value);
        }
        return 1;
    case audioMasterGetSampleRate:
        DEBUG_HOSTCODE("audioMasterGetSampleRate");
        return (long)timeInfo_.sampleRate;
    case audioMasterGetBlockSize:
        DEBUG_HOSTCODE("audioMasterGetBlockSize");
        return 64; // we override this later anyway
    case audioMasterGetInputLatency:
        DEBUG_HOSTCODE("audioMasterGetInputLatency");
        break;
    case audioMasterGetOutputLatency:
        DEBUG_HOSTCODE("audioMasterGetOutputLatency");
        break;
    case audioMasterGetCurrentProcessLevel:
        DEBUG_HOSTCODE("audioMasterGetCurrentProcessLevel");
        if (UIThread::isCurrentThread()){
            return kVstProcessLevelUser;
        } else
            return kVstProcessLevelRealtime;
    case audioMasterGetAutomationState:
        DEBUG_HOSTCODE("audioMasterGetAutomationState");
        break;
    case audioMasterVendorSpecific:
        DEBUG_HOSTCODE("vendor specific");
        break;
    case audioMasterGetDirectory:
        DEBUG_HOSTCODE("audioMasterGetDirectory");
        break;
    case audioMasterUpdateDisplay:
        DEBUG_HOSTCODE("audioMasterUpdateDisplay");
        break;
    case audioMasterBeginEdit:
        DEBUG_HOSTCODE("audioMasterBeginEdit");
        break;
    case audioMasterEndEdit:
        DEBUG_HOSTCODE("audioMasterEndEdit");
        break;
    case audioMasterOpenFileSelector:
        DEBUG_HOSTCODE("audioMasterOpenFileSelector");
        break;
    case audioMasterCloseFileSelector:
        DEBUG_HOSTCODE("audioMasterCloseFileSelector");
        break;
    default:
    #if DEBUG_HOSTCODE_IMPLEMENTATION
        LOG_DEBUG("plugin requested unknown/deprecated opcode " << opcode);
    #endif
        return 0;
    }
    return 0; // ?
}

} // vst
