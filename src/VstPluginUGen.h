#include "SC_PlugIn.hpp"
#include "VSTPluginInterface.h"

#ifndef VSTTHREADS
#define VSTTHREADS 1
#endif

#if VSTTHREADS
#include <atomic>
#include <thread>
#include <future>
#endif

class VstPluginUGen : public SCUnit {
public:
	const uint32 FlagVstGui = 1;
	const uint32 FlagParamDisplay = 2;

	VstPluginUGen();
	~VstPluginUGen();
	bool check();
    void open(const char *path, uint32 flags);
	void close();
	void showEditor(bool show);
	void reset();
	void next(int inNumSamples);
	// param
	void setParam(int32 index, float value);
	void mapParam(int32 index, int32 bus);
	// program/bank
	void setProgram(int32 index);
	void setProgramName(const char *name);
	void readProgram(const char *path);
	void writeProgram(const char *path);
	void setProgramData(const char *data, int32 n);
	void getProgramData();
	void readBank(const char *path);
	void writeBank(const char *path);
	void setBankData(const char *data, int32 n);
	void getBankData();
	// midi
	void sendMidiMsg(int32 status, int32 data1, int32 data2);
	void sendSysexMsg(const char *data, int32 n);
	// transport
	void setTempo(float bpm);
	void setTimeSig(int32 num, int32 denom);
	void setTransportPlaying(bool play);
	void setTransportPos(float pos);
	void getTransportPos();
private:
	struct Param {
		float value;
		int32 bus;
    };
    IVSTPlugin* tryOpenPlugin(const char *path, bool gui);
    // helper methods
    float readControlBus(int32 num, int32 maxChannel);
    void resizeBuffer();
	void sendPluginInfo();
	void sendPrograms();
	bool sendProgram(int32 num);
	void sendParameters();
	void sendMsg(const char *cmd, float f);
	void sendMsg(const char *cmd, int n, const float *data);
	// data members
	IVSTPlugin *plugin_ = nullptr;
    float *buf_ = nullptr;

    int numInChannels_ = 0;
    int32 inBusNum_ = 0;
    float *inBus_ = nullptr;
    int32 *inBusTouched_ = nullptr;
	float **inBufVec_ = nullptr;

    int numOutChannels_ = 0;
    int32 outBusNum_ = 0;
    float *outBus_ = nullptr;
    int32 *outBusTouched_ = nullptr;
	float **outBufVec_ = nullptr;

    Param *paramVec_ = nullptr;
    bool vstGui_ = false;
	bool paramDisplay_ = false;
	bool bypass_ = false;
    std::unique_ptr<IVSTWindow> window_;
    // threading
#if VSTTHREADS
    void threadFunction(std::promise<IVSTPlugin *> promise, const char *path);
    std::thread thread_;
    std::mutex mutex_;
#endif
};

template<typename T>
T clip(T in, T lo, T hi) {
	return std::max<T>(lo, std::min<T>(hi, in));
}

template <bool LockShared>
struct AudioBusGuard
{
    AudioBusGuard(const Unit * unit, int32 currentChannel, int32 maxChannel):
        unit(unit),
        mCurrentChannel(currentChannel),
        isValid(currentChannel < maxChannel)
    {
        if (isValid)
            lock();
    }

    ~AudioBusGuard()
    {
        if (isValid)
            unlock();
    }

    void lock()
    {
        if (LockShared)
            ACQUIRE_BUS_AUDIO_SHARED(mCurrentChannel);
        else
            ACQUIRE_BUS_AUDIO(mCurrentChannel);
    }

    void unlock()
    {
        if (LockShared)
            RELEASE_BUS_AUDIO_SHARED(mCurrentChannel);
        else
            RELEASE_BUS_AUDIO(mCurrentChannel);
    }

    const Unit * unit;
    const int32 mCurrentChannel;
    const bool isValid;
};
