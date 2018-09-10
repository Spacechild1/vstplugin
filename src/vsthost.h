#pragma once

/*
 VST host
 */
 
 #include "m_pd.h" 
 
// VST2
#include <windows.h>
#include <process.h>
#include "VST_SDK/VST2_SDK/pluginterfaces/vst2.x/aeffect.h"
#include "VST_SDK/VST2_SDK/pluginterfaces/vst2.x/aeffectx.h"
#include "VST_SDK/VST2_SDK/pluginterfaces/vst2.x/vstfxstore.h"

#include <vector>
#include <string>
#include <unordered_map>

// Plugin's entry point
typedef AEffect *(*vstPluginFuncPtr)(audioMasterCallback);

// AEffectDispatcherProc
// AEffectProcessProc
// AEffectSetParameterProc
// AEffectGetParameterProc

class VSTHost {
public:
	VSTHost(int nin, int nout);
	~VSTHost();
	
	void perform(int n);
	void setSampleRate(float sr);
	void setBlockSize(int n);
	void setInputBuffer(int chn, float* buf);
	void setOutputBuffer(int chn, float* buf);
	int getNumHostInputs() const;
	int getNumHostOutputs() const;
	int getNumPluginInputs() const;
	int getNumPluginOutputs() const;
	
	void resume();
	void pause();
	void setBypass(bool bypass);
	
	void openPlugin(const std::string& path);
	void closePlugin();
	bool hasPlugin() const;
	
	void showEditor();
	void hideEditor();
	
	void setParameter(int index, float value);
	float getParameter(int index) const;
	int getNumParameters() const;
	std::string getParameterName(int index) const;
	
	void setProgram(int program);
	int getProgram();
	int getNumPrograms() const;
	std::string getProgramName() const;
	void setProgramName(const std::string& name);
	
	int getVstVersion() const;
private:
	VstIntPtr dispatch(VstInt32 opCode,
		VstInt32 index, VstInt32 value, void *ptr, float opt) const;
	void process(float **inputs,
		float **outputs, VstInt32 sampleFrames);
	void updateBuffers();
	static VstIntPtr VSTCALLBACK hostCallback(AEffect *plugin, VstInt32 opcode, 
		VstInt32 index, VstInt32 value, void *ptr, float opt);
	void setupParameters();
	// audio
	std::vector<float*> input_;
	std::vector<float*> output_;
	std::vector<float> inBuffer_;
	std::vector<float*> inBufferVec_;
	std::vector<float> outBuffer_;
	std::vector<float*> outBufferVec_;
	AEffect *plugin_ = nullptr;
	float samplerate_ = 44100;
	int blocksize_ = 64;
	bool bypass_ = false;
	// parameters
	std::unordered_map<int, VstParameterProperties> paramProps_;
};
