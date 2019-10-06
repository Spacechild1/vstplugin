#pragma once

#include "SC_PlugIn.hpp"
#include "Interface.h"
#include "PluginManager.h"
#include "Utility.h"
#include "rt_shared_ptr.hpp"

using namespace vst;

#include <thread>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>

const size_t MAX_OSC_PACKET_SIZE = 1600;

class VSTPlugin;
class VSTPluginDelegate;

// do we set parameters in the NRT thread?
// seemed to work with VST2 but pointless with VST3...
#define NRT_PARAM_SET 0

struct CmdData {
    template<typename T>
    static T* create(World * world, int size = 0);

    rt::shared_ptr<VSTPluginDelegate> owner;
    bool alive() const;
};

struct PluginCmdData : CmdData {
    // for asynchronous commands
    static PluginCmdData* create(World *world, const char* path = 0);
    // data
    void* freeData = nullptr;
    IPlugin::ptr plugin;
    std::thread::id threadID;
    // generic int value
    int value = 0;
    // flexible array for RT memory
    int size = 0;
    char buf[1];
};

struct ParamCmdData : CmdData {
    int index;
    float value;
    // flexible array
    char display[1];
};

struct VendorCmdData : CmdData {
    int32 index;
    int32 value;
    float opt;
    size_t size;
    char data[1];
};

namespace SearchFlags {
    const int useDefault = 1;
    const int verbose = 2;
    const int save = 4;
    const int parallel = 8;
};

struct InfoCmdData : CmdData {
    static InfoCmdData* create(World* world, const char* path);
    static InfoCmdData* create(World* world, int bufnum);
    static bool nrtFree(World* world, void* cmdData);
    int32 flags = 0;
    int32 bufnum = -1;
    void* freeData = nullptr;
    char path[256];
    // flexible array
    int size = 0;
    char buf[1];
};

// This class contains all the state that is shared between the UGen (VSTPlugin) and asynchronous commands.
// It is managed by a rt::shared_ptr and therefore kept alive during the execution of commands, which means
// we don't have to worry about the actual UGen being freed concurrently while a command is still running.
// In the RT stage we can call alive() to verify that the UGen is still alive and synchronize the state.
// We must not access the actual UGen during a NRT stage!

class VSTPluginDelegate :
    public IPluginListener,
    public std::enable_shared_from_this<VSTPluginDelegate>
{
    friend class VSTPlugin;
public:
    VSTPluginDelegate(VSTPlugin& owner);
    ~VSTPluginDelegate();

    bool alive() const;
    void setOwner(VSTPlugin *owner);
    World* world() { return world_;  }
    float sampleRate() const { return sampleRate_; }
    int32 bufferSize() const { return bufferSize_; }
    int32 numInChannels() const { return numInChannels_; }
    int32 numOutChannels() const { return numOutChannels_; }
    int32 numAuxInChannels() const { return numAuxInChannels_; }
    int32 numAuxOutChannels() const { return numAuxOutChannels_; }

    void parameterAutomated(int index, float value) override;
    void midiEvent(const MidiEvent& midi) override;
    void sysexEvent(const SysexEvent& sysex) override;

    // plugin
    IPlugin* plugin() { return plugin_.get(); }
    bool check();
    void open(const char* path, bool gui);
    void doneOpen(PluginCmdData& msg);
    void close();
    void showEditor(bool show);
    void reset(bool async = false);
    // param
    void setParam(int32 index, float value);
    void setParam(int32 index, const char* display);
    void setParamDone(int32 index);
    void queryParams(int32 index, int32 count);
    void getParam(int32 index);
    void getParams(int32 index, int32 count);
    void mapParam(int32 index, int32 bus, bool audio = false);
    void unmapParam(int32 index);
    void unmapAll();
    // program/bank
    void setProgram(int32 index);
    void setProgramName(const char* name);
    void queryPrograms(int32 index, int32 count);
    template<bool bank>
    void readPreset(const char* path);
    template<bool bank>
    void readPreset(int32 buf);
    template<bool bank>
    void writePreset(const char* path);
    template<bool bank>
    void writePreset(int32 buf);
    // midi
    void sendMidiMsg(int32 status, int32 data1, int32 data2, float detune = 0.f);
    void sendSysexMsg(const char* data, int32 n);
    // transport
    void setTempo(float bpm);
    void setTimeSig(int32 num, int32 denom);
    void setTransportPlaying(bool play);
    void setTransportPos(float pos);
    void getTransportPos();
    // advanced
    void canDo(const char* what);
    void vendorSpecific(int32 index, int32 value, size_t size,
        const char* data, float opt, bool async);
    // node reply
    void sendMsg(const char* cmd, float f);
    void sendMsg(const char* cmd, int n, const float* data);
    // helper functions
    bool sendProgramName(int32 num); // unchecked
    void sendCurrentProgramName();
    void sendParameter(int32 index); // unchecked
    void sendParameterAutomated(int32 index, float value); // unchecked
    // perform sequenced command
    template<bool owner = true, typename T>
    void doCmd(T* cmdData, AsyncStageFn stage2, AsyncStageFn stage3 = nullptr,
        AsyncStageFn stage4 = nullptr);
private:
    VSTPlugin *owner_ = nullptr;
    IPlugin::ptr plugin_;
    bool editor_ = false;
    bool isLoading_ = false;
    World* world_ = nullptr;
    // cache (for cmdOpen)
    double sampleRate_ = 1;
    int bufferSize_ = 0;
    int numInChannels_ = 0;
    int numOutChannels_ = 0;
    int numAuxInChannels_ = 0;
    int numAuxOutChannels_ = 0;
    // thread safety
    std::thread::id rtThreadID_;
#if NRT_PARAM_SET
    std::thread::id nrtThreadID_;
#else
    bool bParamSet_ = false; // did we just set a parameter manually?
#endif
};

class VSTPlugin : public SCUnit {
    friend class VSTPluginDelegate;
    friend struct PluginCmdData;
    static const uint32 MagicInitialized = 0x7ff05554; // signalling NaN
    static const uint32 MagicQueued = 0x7ff05555; // signalling NaN
public:
    VSTPlugin();
    ~VSTPlugin();
    bool initialized();
    void queueUnitCmd(UnitCmdFunc fn, sc_msg_iter* args);
    void runUnitCmds();
    VSTPluginDelegate& delegate() { return *delegate_;  }

    void next(int inNumSamples);
    int getFlags() const { return (int)in0(1); } // not used (yet)
    int getBypass() const { return (int)in0(2); }
    int numInChannels() const { return (int)in0(3); }
    int numAuxInChannels() const {
        return (int)in0(auxInChannelOnset_ - 1);
    }
    int numOutChannels() const { return (int)in0(0); }
    int numAuxOutChannels() const { return numOutputs() - numOutChannels(); }

    int numParameterControls() const { return (int)in0(parameterControlOnset_ - 1); }
    void update();
    void map(int32 index, int32 bus, bool audio);
    void unmap(int32 index);
    void clearMapping();
private:
    float readControlBus(uint32 num);
    // data members
    volatile uint32 initialized_ = MagicInitialized; // set by constructor
    volatile uint32 queued_; // set to MagicQueued when queuing unit commands
    struct UnitCmdQueueItem {
        UnitCmdQueueItem *next;
        UnitCmdFunc fn;
        int32 size;
        char data[1];
    };
    UnitCmdQueueItem *unitCmdQueue_; // initialized *before* constructor

    rt::shared_ptr<VSTPluginDelegate> delegate_;

    static const int inChannelOnset_ = 4;
    int auxInChannelOnset_ = 0;
    int parameterControlOnset_ = 0;

    struct Mapping {
        enum BusType {
            Control = 0,
            Audio
        };
        void setBus(uint32 num, BusType type) {
            // use last bit in bus to encode the type
            bus_ = num | (static_cast<uint32>(type) << 31);
        }
        BusType type() const {
            return static_cast<BusType>(bus_ >> 31);
        }
        uint32 bus() const {
            return bus_ & 0x7FFFFFFF;
        }
        Mapping* prev;
        Mapping* next;
        uint32 index;
    private:
        uint32 bus_;
    };
    Mapping* paramMappingList_ = nullptr;
    float* paramState_ = nullptr;
    Mapping** paramMapping_ = nullptr;
    Bypass bypass_ = Bypass::Off;

    // threading
#if HAVE_UI_THREAD
    struct ParamChange {
        int index;
        float value;
    };
    LockfreeFifo<ParamChange, 16> paramQueue_;
    std::mutex paramQueueMutex_; // for writers
#endif
};



