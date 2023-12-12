#pragma once

#include "SC_PlugIn.hpp"

#include "Interface.h"
#include "PluginDictionary.h"
#include "FileUtils.h"
#include "MiscUtils.h"
#include "Lockfree.h"
#include "Log.h"
#include "Sync.h"
#include "CpuArch.h"

using namespace vst;

#include <thread>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>

const size_t MAX_OSC_PACKET_SIZE = 1600;

class VSTPlugin;
class OpenCmdData;

// This class contains all the state that is shared between the UGen (VSTPlugin) and asynchronous commands.
// It is managed by a rt::shared_ptr and therefore kept alive during the execution of commands, which means
// we don't have to worry about the actual UGen being freed concurrently while a command is still running.
// In the RT stage we can call alive() to verify that the UGen is still alive and synchronize the state.
// We must not access the actual UGen during a NRT stage!

class VSTPluginDelegate : public IPluginListener
{
    friend class VSTPlugin;
    friend class VSTPluginDelegatePtr;
public:
    enum EventType {
        LatencyChange = -3,
        UpdateDisplay,
        PluginCrash
    };

    VSTPluginDelegate(VSTPlugin& owner);
    ~VSTPluginDelegate();

    bool alive() const;
    void setOwner(VSTPlugin *owner);
    World* world() { return world_; }

    void parameterAutomated(int index, float value) override;
    void latencyChanged(int nsamples) override;
    void updateDisplay() override;
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
    void doClose();

    void showEditor(bool show);
    void setEditorPos(int x, int y);
    void setEditorSize(int w, int h);

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
    void sendUpdateDisplay();
    void sendPluginCrash();

    // perform sequenced command
    template<typename T>
    void doCmd(T* cmdData, AsyncStageFn stage2, AsyncStageFn stage3 = nullptr,
        AsyncStageFn stage4 = nullptr);

    // misc
    bool hasEditor() const {
        return editor_;
    }
    void update();
    void handleEvents();
private:
    std::atomic<int32_t> refcount_{0}; // doesn't really have to be atomic...
    VSTPlugin *owner_ = nullptr;
    IPlugin::ptr plugin_;
    World* world_ = nullptr;
    bool editor_ = false;
    bool threaded_ = false;
    bool isLoading_ = false;
    bool paramSet_ = false; // did we just set a parameter manually?
    bool suspended_ = false;
    Mutex mutex_; // actually, this could probably be a spinlock...
    // events
    struct ParamChange {
        int index; // parameter index or EventType (negative)
        float value;
    };
    // don't use RT allocator! only non-realtime threads are allowed
    // to push to the queue. Also, the internal queue memory (if any)
    // has to be disposed in the NRT thread, see ~VSTPluginDelegate().
    using ParamQueue = UnboundedMPSCQueue<ParamChange>;
    ParamQueue* paramQueue_;

    void addRef();
    void release();
};

class VSTPluginDelegatePtr {
public:
    VSTPluginDelegatePtr(VSTPluginDelegate *ptr = nullptr)
        : ptr_(ptr) {
        if (ptr) {
            ptr->addRef();
        }
    }

    VSTPluginDelegatePtr(const VSTPluginDelegatePtr& other) {
        ptr_ = other.ptr_;
        if (ptr_) {
            ptr_->addRef();
        }
    }

    VSTPluginDelegatePtr(VSTPluginDelegatePtr&& other) {
        ptr_ = other.ptr_;
        other.ptr_ = nullptr;
        // refcount stays the same
    }

    ~VSTPluginDelegatePtr() {
        if (ptr_) {
            ptr_->release();
        }
    }

    VSTPluginDelegatePtr& operator=(VSTPluginDelegatePtr&& other) {
        if (ptr_) {
            ptr_->release();
        }
        ptr_ = other.ptr_;
        other.ptr_ = nullptr;
        // refcount stays the same
        return *this;
    }

    VSTPluginDelegatePtr& operator=(const VSTPluginDelegatePtr& other) {
        if (ptr_) {
            ptr_->release();
        }
        ptr_ = other.ptr_;
        if (ptr_) {
            ptr_->addRef();
        }
        return *this;
    }

    void reset(VSTPluginDelegate *ptr) {
        if (ptr_) {
            ptr_->release();
        }
        ptr_ = ptr;
        if (ptr) {
            ptr->addRef();
        }
    }

    VSTPluginDelegate* get() const { return ptr_; }

    VSTPluginDelegate& operator*() const { return *ptr_; }

    VSTPluginDelegate* operator->() const { return ptr_; }

    operator bool () const { return ptr_ != nullptr; }

    bool operator==(const VSTPluginDelegatePtr& other) const { return ptr_ == other.ptr_; }

    bool operator!=(const VSTPluginDelegatePtr& other) const { return ptr_ != other.ptr_; }
private:
    VSTPluginDelegate *ptr_;
};

class VSTPlugin : public SCUnit {
    friend class VSTPluginDelegate;
    friend struct PluginCmdData;
public:
    VSTPlugin();
    ~VSTPlugin();
    // HACK to check if the class has been fully constructed. See "runUnitCommand".
    bool initialized() const {
        return mSpecialIndex & Initialized;
    }
    bool valid() const {
        return mSpecialIndex & Valid;
    }
    void queueUnitCmd(UnitCmdFunc fn, sc_msg_iter* args);
    void runUnitCmds();
    VSTPluginDelegate& delegate() { return *delegate_;  }

    void next(int inNumSamples);

    int getBypass() const { return (int)in0(2); }

    int blockSize() const;

    int reblockPhase() const;

    struct Bus {
        float **channelData = nullptr;
        int numChannels = 0;
    };

    int numInputBusses() const {
        return numUgenInputs_;
    }
    int numOutputBusses() const {
        return numUgenOutputs_;
    }
    const Bus * inputBusses() const {
        return ugenInputs_;
    }
    const Bus * outputBusses() const {
        return ugenOutputs_;
    }

    void map(int32 index, int32 bus, bool audio);
    void unmap(int32 index);
    void clearMapping();

    void setupPlugin(const int *inputs, int numInputs,
                     const int *outputs, int numOutputs);
private:
    void setInvalid() { mSpecialIndex &= ~Valid; }

    float readControlBus(uint32 num);

    bool setupBuffers(AudioBus *& pluginBusses, int& pluginBusCount,
                      int& totalNumChannels, Bus *ugenBusses, int ugenBusCount,
                      const int *speakers, int numSpeakers, float *dummy);

    void initReblocker(int reblockSize);
    bool updateReblocker(int numSamples);
    void freeReblocker();

    void performBypass(const Bus *ugenInputs, int numInputs,
                       int numSamples, int phase);

    static const int Initialized = 1;
    static const int UnitCmdQueued = 2;
    static const int Valid = 4;
    // data members
    struct UnitCmdQueueItem {
        UnitCmdQueueItem *next;
        UnitCmdFunc fn;
        int32 size;
        char data[1];
    };
    UnitCmdQueueItem *unitCmdQueue_; // initialized *before* constructor

    VSTPluginDelegatePtr delegate_;

    int numUgenInputs_ = 0;
    int numUgenOutputs_ = 0;
    int numPluginInputs_ = 0;
    int numPluginOutputs_ = 0;
    int numPluginInputChannels_ = 0;
    int numPluginOutputChannels_ = 0;
    Bus *ugenInputs_ = nullptr;
    Bus *ugenOutputs_ = nullptr;
    AudioBus *pluginInputs_ = nullptr;
    AudioBus *pluginOutputs_ = nullptr;
    float *dummyBuffer_ = nullptr;

    struct Reblock {
        int blockSize;
        int phase;
        int numInputs;
        int numOutputs;
        Bus *inputs;
        Bus *outputs;
        float *buffer;
    };

    Reblock *reblock_ = nullptr;

    int numParameterControls_ = 0;
    Wire ** parameterControls_ = nullptr;

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

    void printMapping();
};

class VSTPluginDelegate;

struct CmdData {
    template<typename T>
    static T* create(World * world, int size = 0);

    VSTPluginDelegatePtr owner;
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
    RunMode runMode;
    double sampleRate;
    int blockSize;
    ProcessMode processMode;
    int numInputs;
    int numOutputs;
    int *inputs;
    int *outputs;
    std::vector<int> pluginInputs;
    std::vector<int> pluginOutputs;
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

struct SetupCmdData : CmdData {
    double sampleRate;
    int blockSize;
    ProcessMode processMode;
};

struct WindowCmdData : CmdData {
    union {
        int x;
        int width;
    };
    union {
        int y;
        int height;
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
    const int verbose = 1;
    const int save = 2;
    const int parallel = 4;
}

struct SearchCmdData {
    static bool nrtFree(World* world, void* cmdData);
    int32 flags = 0;
    int32 bufnum = -1;
    float timeout = 0.0;
    void* freeData = nullptr;
    char path[256];
    char cacheFileDir[256];
    int32 numSearchPaths = 0;
    int32 numExcludePaths = 0;
    // flexibel struct member
    union {
        char *pathList[1];
        char pathBuf[1];
    };
};

