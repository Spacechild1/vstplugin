#pragma once

#include "SC_PlugIn.hpp"
#include "VSTPluginInterface.h"

#ifndef VSTTHREADS
#define VSTTHREADS 1
#endif

#if VSTTHREADS
#include <thread>
#include <future>
#endif

#include <memory>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>

const size_t MAX_OSC_PACKET_SIZE = 1600;

enum VstPluginFlags {
	HasEditor = 0,
	IsSynth,
	SinglePrecision,
	DoublePrecision,
	MidiInput,
	MidiOutput,
	SysexInput,
	SysexOutput
};

struct VstPluginInfo {
	void serialize(std::ofstream& file);
	// data
	std::string key;
	std::string name;
	std::string fullPath;
	int version = 0;
	int id = 0;
	int numInputs = 0;
	int numOutputs = 0;
	// parameter name + label
	std::vector<std::pair<std::string, std::string>> parameters;
	// default programs
	std::vector<std::string> programs;
	// see VstPluginFlags
	uint32 flags = 0;
};

using VstPluginMap = std::unordered_map<std::string, VstPluginInfo>;

class VstPlugin;

struct VstPluginCmdData {
	void tryOpen();
	void close();
	// data
	VstPlugin *owner;
	IVSTPlugin *plugin = nullptr;
	std::shared_ptr<IVSTWindow> window;
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
	VstPlugin *owner;
	int index;
	float value;
	// flexible array
	char display[1];
};

struct QueryCmdData {
	char reply[1600];
	int value;
	int index;
	// flexible array
	char buf[1];
};

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
	friend struct VstPluginCmdData;
	static const uint32 MagicNumber = 0x5da815bc;
public:
	VstPlugin();
	~VstPlugin();
	IVSTPlugin *plugin();
	bool check();
	bool valid();
    void open(const char *path, bool gui);
	void doneOpen(VstPluginCmdData& msg);
	void close();
	void showEditor(bool show);
	void reset(bool async = false);
	void next(int inNumSamples);
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
	void readBank(const char *path);
	void sendProgramData(int32 totalSize, int32 onset, const char *data, int32 n) {
		sendData(totalSize, onset, data, n, false);
	}
	void sendBankData(int32 totalSize, int32 onset, const char *data, int32 n) {
		sendData(totalSize, onset, data, n, true);
	}
	void writeProgram(const char *path);
	void writeBank(const char *path);
	void receiveProgramData(int count);
	void receiveBankData(int count);
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
	void vendorSpecific(int32 index, int32 value, void *ptr, float opt);
	// node reply
	void sendMsg(const char *cmd, float f);
	void sendMsg(const char *cmd, int n, const float *data);
	struct Param {
		float value;
		int32 bus;
    };
    // helper methods
    float readControlBus(int32 num, int32 maxChannel);
    void resizeBuffer();
	bool sendProgramName(int32 num); // unchecked
	void sendCurrentProgramName();
	void sendParameter(int32 index); // unchecked
	void parameterAutomated(int32 index, float value);
	void midiEvent(const VSTMidiEvent& midi);
	void sysexEvent(const VSTSysexEvent& sysex);
	void sendData(int32 totalSize, int32 onset, const char *data, int32 n, bool bank);
	// for asynchronous commands
    VstPluginCmdData* makeCmdData(const char *s);
	VstPluginCmdData* makeCmdData(const char *data, size_t size);
	VstPluginCmdData* makeCmdData(size_t size);
	VstPluginCmdData* makeCmdData();
	template<typename T>
	void doCmd(T *cmdData, AsyncStageFn nrt, AsyncStageFn rt=nullptr);
	static bool cmdGetData(World *world, void *cmdData, bool bank);
	static bool cmdGetDataDone(World *world, void *cmdData, bool bank);
private:
	// data members
	uint32 magic_ = MagicNumber;
	IVSTPlugin *plugin_ = nullptr;
	bool isLoading_ = false;
	bool bypass_ = false;
	std::shared_ptr<IVSTWindow> window_;
	std::unique_ptr<VstPluginListener> listener_;

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
	std::thread::id rtThreadID_;
    std::mutex mutex_;
	std::vector<std::pair<int, float>> paramQueue_;
#endif

	// send program/bank data
	std::string dataNRT_;
	int32 dataSent_ = 0;
	// receive program/bank data
	char *dataRT_ = nullptr;
	int32 dataSize_ = 0;
	int32 dataReceived_ = 0;
};



