#pragma once

#include "SC_PlugIn.hpp"
#include "Interface.h"
#include "PluginManager.h"
#include "Utility.h"
#include "Sync.h"
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

struct CmdData {
    template<typename T>
    static T* create(World * world, int size = 0);

    rt::shared_ptr<VSTPluginDelegate> owner;
    bool alive() const;
};

struct CloseCmdData : CmdData {
    IPlugin::ptr plugin;
    bool editor;
};

// cache all relevant info so we don't have to touch
// the VSTPlugin instance during the async command.
struct OpenCmdData : CmdData {
    IPlugin::ptr plugin;
    bool editor;
    bool threaded;
    RunMode mode;
    double sampleRate;
    int blockSize;
    int numInputs;
    int numAuxInputs;
    int numOutputs;
    int numAuxOutputs;
    // flexible array for RT memory
    int size = 0;
    char path[1];
};

struct PluginCmdData : CmdData {
    union {
        int i;
        float f;
    };
};

struct VendorCmdData : CmdData {
    int32 index;
    int32 value;
    float opt;
    size_t size;
    char data[1];
};

struct PresetCmdData : CmdData {
    static PresetCmdData* create(World* world, const char* path, bool async = false);
    static PresetCmdData* create(World* world, int bufnum, bool async = false);
    static bool nrtFree(World* world, void* cmdData);
    std::string buffer;
    int result = 0;
    int32 bufnum = -1;
    bool async = false;
    void* freeData = nullptr;
    // flexible array
    char path[1];
};

namespace SearchFlags {
    const int useDefault = 1;
    const int verbose = 2;
    const int save = 4;
    const int parallel = 8;
}

struct SearchCmdData {
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
    enum EventType {
        LatencyChange = -2,
        PluginCrash
    };

    VSTPluginDelegate(VSTPlugin& owner);
    ~VSTPluginDelegate();

    bool alive() const;
    void setOwner(VSTPlugin *owner);
    World* world() { return world_; }

    void parameterAutomated(int index, float value) override;
    void latencyChanged(int nsamples) override;
    void pluginCrashed() override;
    void midiEvent(const MidiEvent& midi) override;
    void sysexEvent(const SysexEvent& sysex) override;

    // plugin
    IPlugin* plugin() { return plugin_.get(); }
    bool check(bool loud = true) const;
    bool isSuspended() const { return suspended_; }
    void suspend() { suspended_ = true; }
    void resume() { suspended_ = false; }
    Lock scopedLock();
    bool tryLock();
    void unlock();
    void open(const char* path, bool editor,
              bool threaded, RunMode mode);
    void doneOpen(OpenCmdData& msg);
    void close();
    void showEditor(bool show);
    void reset(bool async);
    // param
    void setParam(int32 index, float value);
    void setParam(int32 index, const char* display);
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
    template<bool bank, typename T>
    void readPreset(T dest, bool async);
    template<bool bank, typename T>
    void writePreset(T dest, bool async);
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
    void sendParameter(int32 index, float value); // unchecked
    void sendParameterAutomated(int32 index, float value); // unchecked
    int32 latencySamples() const;
    void sendLatencyChange(int nsamples);
    void sendPluginCrash();
    // perform sequenced command
    template<typename T>
    void doCmd(T* cmdData, AsyncStageFn stage2, AsyncStageFn stage3 = nullptr,
        AsyncStageFn stage4 = nullptr);
    bool hasEditor() const {
        return editor_;
    }
private:
    VSTPlugin *owner_ = nullptr;
    IPlugin::ptr plugin_;
    bool editor_ = false;
    bool threaded_ = false;
    bool isLoading_ = false;
    World* world_ = nullptr;
    // thread safety
    std::thread::id rtThreadID_;
    bool paramSet_ = false; // did we just set a parameter manually?
    bool suspended_ = false;
    Mutex mutex_;
};

class VSTPlugin : public SCUnit {
    friend class VSTPluginDelegate;
    friend struct PluginCmdData;
public:
    VSTPlugin();
    ~VSTPlugin();
    bool initialized();
    void queueUnitCmd(UnitCmdFunc fn, sc_msg_iter* args);
    void runUnitCmds();
    VSTPluginDelegate& delegate() { return *delegate_;  }

    void next(int inNumSamples);
    int getBypass() const { return (int)in0(2); }

    int blockSize() const;

    int numInChannels() const { return (int)in0(3); }
    int numAuxInChannels() const {
        return (int)in0(auxInChannelOnset_ - 1);
    }
    int numOutChannels() const { return (int)in0(0); }
    int numAuxOutChannels() const { return numOutputs() - numOutChannels(); }

    int numParameterControls() const { return (int)in0(parameterControlOnset_ - 1); }

    void map(int32 index, int32 bus, bool audio);
    void unmap(int32 index);
    void clearMapping();

    void update();
private:
    float readControlBus(uint32 num);
    static const int Initialized = 1;
    static const int UnitCmdQueued = 2;
    // data members
    struct UnitCmdQueueItem {
        UnitCmdQueueItem *next;
        UnitCmdFunc fn;
        int32 size;
        char data[1];
    };
    UnitCmdQueueItem *unitCmdQueue_; // initialized *before* constructor

    rt::shared_ptr<VSTPluginDelegate> delegate_;

    struct Reblock {
        int blockSize;
        int phase;
        float **input;
        float **auxInput;
        float **output;
        float **auxOutput;
        float *buffer;
    };

    Reblock *reblock_ = nullptr;

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
    struct ParamChange {
        int index; // parameter index or EventType (negative)
        float value;
    };
    LockfreeFifo<ParamChange, 16> paramQueue_;
    Mutex paramQueueMutex_; // for writers
};



