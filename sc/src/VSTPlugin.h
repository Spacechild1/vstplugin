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
    void mapParam(int32 index, int32 bus);
    void unmapParam(int32 index);
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
    void sendMidiMsg(int32 status, int32 data1, int32 data2);
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
    void ref();
    void unref();
private:
    VSTPlugin *owner_ = nullptr;
    IPlugin::ptr plugin_;
    bool editor_ = false;
    bool isLoading_ = false;
    int pluginUseCount_ = 0; // plugin currently used by asynchronuous commands?
    World* world_ = nullptr;
    // cache (for cmdOpen)
    double sampleRate_ = 1;
    int bufferSize_ = 0;
    int numInChannels_ = 0;
    int numOutChannels_ = 0;
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
    int numInChannels() const { return numInChannels_; }
    int numOutChannels() const { return numOutputs(); }

    void update();
    void map(int32 index, int32 bus);
    void unmap(int32 index);
private:
    void resizeBuffer();
    void clearMapping();
    float readControlBus(int32 num);
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

    bool bypass_ = false;
    rt::shared_ptr<VSTPluginDelegate> delegate_;

    static const int inChannelOnset_ = 2;
    int numInChannels_ = 0;
    float *buf_ = nullptr;
    const float **inBufVec_ = nullptr;
    float **outBufVec_ = nullptr;
    struct Mapping {
        int32 index;
        int32 bus;
        Mapping* prev;
        Mapping* next;
    };
    Mapping* paramMappingList_ = nullptr;
    float* paramState_ = nullptr;
    Mapping** paramMapping_ = nullptr;
    int numParameterControls_ = 0;
    int parameterControlOnset_ = 0;

    // threading
#if VSTTHREADS
    std::mutex mutex_;
    std::vector<std::pair<int, float>> paramQueue_;
#endif
};



