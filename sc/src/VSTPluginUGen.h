#pragma once

#include "SC_PlugIn.hpp"
#include "VSTPluginInterface.h"
#include "VSTPluginManager.h"
#include "Utility.h"

using namespace vst;

#ifndef VSTTHREADS
#define VSTTHREADS 1
#endif

#if VSTTHREADS
#include <future>
#endif

#include <thread>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>

const size_t MAX_OSC_PACKET_SIZE = 1600;

using VSTPluginMap = std::unordered_map<std::string, VSTPluginDesc>;

class VSTPlugin;

struct VSTPluginCmdData {
    // for asynchronous commands
    static VSTPluginCmdData* create(VSTPlugin* owner, const char* path = 0);
    void open();
    void close();
    // data
    VSTPlugin* owner = nullptr;
    void* freeData = nullptr;
    IVSTPlugin::ptr plugin;
    IVSTWindow::ptr window;
    std::thread::id threadID;
#if VSTTHREADS
    std::thread thread;
#endif
    // generic int value
    int value = 0;
    // flexible array for RT memory
    int size = 0;
    char buf[1];
};

struct ParamCmdData {
    VSTPlugin *owner;
    int index;
    float value;
    // flexible array
    char display[1];
};

struct VendorCmdData {
    VSTPlugin *owner;
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
};

struct InfoCmdData {
    static InfoCmdData* create(World* world, int size = 0);
    static InfoCmdData* create(VSTPlugin* owner, const char* path);
    static InfoCmdData* create(VSTPlugin* owner, int bufnum);
    static bool nrtFree(World* world, void* cmdData);
    VSTPlugin* owner = nullptr;
    int32 flags = 0;
    int32 bufnum = -1;
    void* freeData = nullptr;
    char path[256];
    // flexible array
    int size = 0;
    char buf[1];
};

class VSTPluginListener : public IVSTPluginListener {
public:
    VSTPluginListener(VSTPlugin& owner);
    void parameterAutomated(int index, float value) override;
    void midiEvent(const VSTMidiEvent& midi) override;
    void sysexEvent(const VSTSysexEvent& sysex) override;
private:
    VSTPlugin *owner_ = nullptr;
};

class VSTPlugin : public SCUnit {
    friend class VSTPluginListener;
    friend struct VSTPluginCmdData;
    static const uint32 MagicInitialized = 0x7ff05554; // signalling NaN
    static const uint32 MagicQueued = 0x7ff05555; // signalling NaN
public:
    VSTPlugin();
    ~VSTPlugin();
    IVSTPlugin *plugin();
    bool check();
    bool initialized();
    void queueUnitCmd(UnitCmdFunc fn, sc_msg_iter* args);
    void runUnitCmds();

    void open(const char *path, bool gui);
    void doneOpen(VSTPluginCmdData& msg);
    void close();
    void showEditor(bool show);
    void reset(bool async = false);
    void next(int inNumSamples);
    int numInChannels() const { return numInChannels_; }
    int numOutChannels() const { return numOutChannels_;  }
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
    void setProgramName(const char *name);
    void queryPrograms(int32 index, int32 count);
    void readProgram(const char *path);
    void readProgram(int32 buf);
    void readBank(const char *path);
    void readBank(int32 buf);
    void writeProgram(const char *path);
    void writeProgram(int32 buf);
    void writeBank(const char *path);
    void writeBank(int32 buf);
    // midi
    void sendMidiMsg(int32 status, int32 data1, int32 data2);
    void sendSysexMsg(const char *data, int32 n);
    // transport
    void setTempo(float bpm);
    void setTimeSig(int32 num, int32 denom);
    void setTransportPlaying(bool play);
    void setTransportPos(float pos);
    void getTransportPos();
    // advanced
    void canDo(const char *what);
    void vendorSpecific(int32 index, int32 value, size_t size,
        const char *data, float opt, bool async);
    // node reply
    void sendMsg(const char *cmd, float f);
    void sendMsg(const char *cmd, int n, const float *data);
    struct Param {
        float value;
        int32 bus;
    };
    // helper methods
    float readControlBus(int32 num);
    void resizeBuffer();
    bool sendProgramName(int32 num); // unchecked
    void sendCurrentProgramName();
    void sendParameter(int32 index); // unchecked
    void parameterAutomated(int32 index, float value);
    void midiEvent(const VSTMidiEvent& midi);
    void sysexEvent(const VSTSysexEvent& sysex);
    // perform sequenced command
    template<typename T>
    void doCmd(T *cmdData, AsyncStageFn stage2, AsyncStageFn stage3 = nullptr,
        AsyncStageFn stage4 = nullptr);
private:
    // data members
    uint32 initialized_ = MagicInitialized; // set by constructor
    uint32 queued_; // set to MagicQueued when queuing unit commands
    struct UnitCmdQueueItem {
        UnitCmdQueueItem *next;
        UnitCmdFunc fn;
        int32 size;
        char data[1];
    };
    UnitCmdQueueItem *unitCmdQueue_; // initialized *before* constructor

    IVSTPlugin::ptr plugin_ = nullptr;
    bool isLoading_ = false;
    bool bypass_ = false;
    IVSTWindow::ptr window_;
    VSTPluginListener::ptr listener_;

    float *buf_ = nullptr;
    int numInChannels_ = 0;
    static const int inChannelOnset_ = 2;
    const float **inBufVec_ = nullptr;
    int numOutChannels_ = 0;
    float **outBufVec_ = nullptr;
    Param *paramStates_ = nullptr;
    int numParameterControls_ = 0;
    int parameterControlOnset_ = 0;

    // threading
#if VSTTHREADS
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::pair<int, float>> paramQueue_;
#endif
    std::thread::id rtThreadID_;
    std::thread::id nrtThreadID_;
};



