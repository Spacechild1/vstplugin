#include "SC_PlugIn.hpp"
#include "VSTPluginInterface.h"

#ifndef VSTTHREADS
#define VSTTHREADS 1
#endif

#if VSTTHREADS
#include <atomic>
#include <thread>
#include <future>
#include <vector>
#endif

#include <limits>

namespace Flags {
	const uint32 ScGui = 1;
	const uint32 VstGui = 2;
	const uint32 ParamDisplay = 4;
}

enum PluginInfo {
	HasEditor = 0,
	IsSynth,
	SinglePrecision,
	DoublePrecision,
	MidiInput,
	MidiOutput,
	SysexInput,
	SysexOutput
};

class VstPlugin;

class VstPluginListener : public IVSTPluginListener {
public:
	VstPluginListener(VstPlugin& owner);
	void parameterAutomated(int index, float value) override;
	void midiEvent(const VSTMidiEvent& midi) override;
	void sysexEvent(const VSTSysexEvent& sysex) override;
private:
	VstPlugin *owner_ = nullptr;
};

class VstPlugin : public SCUnit {
	friend class VstPluginListener;
	static const uint32 MagicNumber = 0x5da815bc;
public:
	VstPlugin();
	~VstPlugin();
	IVSTPlugin *plugin();
	bool check();
	bool valid();
    void open(const char *path, uint32 flags);
	void close();
	void showEditor(bool show);
	void reset();
	void next(int inNumSamples);
	// param
	void setParam(int32 index, float value);
	void getParam(int32 index);
	void getParamN(int32 index, int32 count);
	void mapParam(int32 index, int32 bus);
	void unmapParam(int32 index);
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
	void sendCurrentProgram();
	void sendParameters();
	void parameterAutomated(int32 index, float value);
	void midiEvent(const VSTMidiEvent& midi);
	void sysexEvent(const VSTSysexEvent& sysex);
	void sendMsg(const char *cmd, float f);
	void sendMsg(const char *cmd, int n, const float *data);

	// data members
	uint32 magic_ = MagicNumber;
	IVSTPlugin *plugin_ = nullptr;
	bool scGui_ = false;
	bool vstGui_ = false;
	bool paramDisplay_ = false;
	bool bypass_ = false;
	std::unique_ptr<IVSTWindow> window_;
	std::unique_ptr<VstPluginListener> listener_;

    float *buf_ = nullptr;
    int numInChannels_ = 0;
	static const int inChannelOnset_ = 2;
	const float **inBufVec_ = nullptr;
    int numOutChannels_ = 0;
	float **outBufVec_ = nullptr;
    Param *paramVec_ = nullptr;
	int numParameterControls_ = 0;
	int parameterControlOnset_ = 0;

    // threading
#if VSTTHREADS
    void threadFunction(std::promise<IVSTPlugin *> promise, const char *path);
    std::thread thread_;
	std::thread::id threadID_;
    std::mutex mutex_;
	std::vector<std::pair<int, float>> paramQueue_;
#endif
};


