#pragma once

#include "SC_PlugIn.hpp"
#include "VSTPluginInterface.h"

#ifndef VSTTHREADS
#define VSTTHREADS 1
#endif

#if VSTTHREADS
#include <thread>
#include <future>
#include <vector>
#endif

#include <memory>

const size_t MAX_OSC_PACKET_SIZE = 1600;

enum GuiType {
	NO_GUI,
	SC_GUI,
	VST_GUI
};

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

struct VstPluginCmdData {
	bool tryOpen();
	void doneOpen();
	void close();
	// data
	VstPlugin *owner_;
	IVSTPlugin *plugin_ = nullptr;
	std::shared_ptr<IVSTWindow> window_;
#if VSTTHREADS
	std::thread thread_;
#endif
	// generic int value
	int value_ = 0;
	// non-realtime memory
	std::string mem_;
	// flexible array for RT memory
	int size_ = 0;
	char buf_[1];
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
    void open(const char *path, GuiType gui);
	void doneOpen(VstPluginCmdData& msg);
	void close();
	void showEditor(bool show);
	void reset(bool nrt = false);
	void next(int inNumSamples);
	// param
	void setParam(int32 index, float value);
	void setParam(int32 index, const char* display);
	void getParam(int32 index);
	void getParamN(int32 index, int32 count);
	void mapParam(int32 index, int32 bus);
	void unmapParam(int32 index);
	void setUseParamDisplay(bool use);
	void setNotifyParamChange(bool use);
	// program/bank
	void setProgram(int32 index);
	void setProgramName(const char *name);
	void readProgram(const char *path);
	void readBank(const char *path);
	void sendProgramData(int32 totalSize, int32 onset, const char *data, int32 n) {
		sendData(totalSize, onset, data, n, false);
	}
	void sendBankData(int32 totalSize, int32 onset, const char *data, int32 n) {
		sendData(totalSize, onset, data, n, true);
	}
	void setProgramData(const char *data, int32 n);
	void setBankData(const char *data, int32 n);
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
private:
	struct Param {
		float value;
		int32 bus;
    };
    // helper methods
    float readControlBus(int32 num, int32 maxChannel);
    void resizeBuffer();
	void sendPluginInfo();
	void sendProgramNames();
	bool sendProgramName(int32 num);
	void sendCurrentProgramName();
	void sendParameter(int32 index); // not checked
	void sendParameters();
	void parameterAutomated(int32 index, float value);
	void midiEvent(const VSTMidiEvent& midi);
	void sysexEvent(const VSTSysexEvent& sysex);
	void sendData(int32 totalSize, int32 onset, const char *data, int32 n, bool bank);
	// for asynchronous commands
    VstPluginCmdData* makeCmdData(const char *s);
	VstPluginCmdData* makeCmdData(const char *data, size_t size);
	VstPluginCmdData* makeCmdData(size_t size);
	VstPluginCmdData* makeCmdData();
	void doCmd(VstPluginCmdData *cmdData, AsyncStageFn nrt,
		AsyncStageFn rt=nullptr);
	static bool cmdGetData(World *world, void *cmdData, bool bank);
	static bool cmdGetDataDone(World *world, void *cmdData, bool bank);
	// data members
	uint32 magic_ = MagicNumber;
	IVSTPlugin *plugin_ = nullptr;
	bool isLoading_ = false;
	GuiType gui_ = NO_GUI;
	bool notify_ = false;
	bool paramDisplay_ = true; // true by default
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



