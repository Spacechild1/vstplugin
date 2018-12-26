#include "SC_PlugIn.hpp"
#include "VSTPluginInterface.h"

static InterfaceTable *ft;

void SCLog(const std::string& msg){
	Print(msg.c_str());
}

class VstPlugin : public SCUnit {
public:
	VstPlugin();
	~VstPlugin();
	void load(const std::string& path);
	void next(int inNumSamples);
private:
	IVSTPlugin *plugin_ = nullptr;
	float *inBuf_ = nullptr;
	float **inBufPtr_ = nullptr;
	float *outBuf_ = nullptr;
	float **outBufPtr_ = nullptr;
};

VstPlugin::VstPlugin(){
	load(TEST_PLUGIN);
	set_calc_function<VstPlugin, &VstPlugin::next>();
}

VstPlugin::~VstPlugin(){
	if (inBuf_) RTFree(mWorld, inBuf_);
	if (inBufPtr_) RTFree(mWorld, inBufPtr_);
	if (outBuf_) RTFree(mWorld, outBuf_);
	if (outBufPtr_) RTFree(mWorld, outBufPtr_);
	if (plugin_) freeVSTPlugin(plugin_);
}

void VstPlugin::load(const std::string& path){
	plugin_ = loadVSTPlugin(makeVSTPluginFilePath(path));
	if (plugin_){
		Print("loaded '%s'!\n", path.c_str());
		int bufSize = bufferSize();
		plugin_->setSampleRate(sampleRate());
		plugin_->setBlockSize(bufSize);
		int nin = plugin_->getNumInputs();
		int nout = plugin_->getNumOutputs();
		// resize buffers (LATER check result of RTRealloc)
		// input buffer
		inBuf_ = (float *)RTRealloc(mWorld, inBuf_, nin * bufSize * sizeof(float)); 
		memset(inBuf_, 0, nin * bufSize * sizeof(float));
		inBufPtr_ = (float **)RTRealloc(mWorld, inBufPtr_, nin * sizeof(float *));
		for (int i = 0; i < nin; ++i){
			inBufPtr_[i] = &inBuf_[i * bufSize];
		}
		// output buffer
		outBuf_ = (float *)RTRealloc(mWorld, outBuf_, nout * bufSize * sizeof(float));
		memset(outBuf_, 0, nout * bufSize * sizeof(float));
		outBufPtr_ = (float **)RTRealloc(mWorld, outBufPtr_, nout * sizeof(float *));
		for (int i = 0; i < nout; ++i){
			outBufPtr_[i] = &outBuf_[i * bufSize];
		}
	} else {
		Print("couldn't load '%s'!\n", path.c_str());
	}
}

void VstPlugin::next(int inNumSamples){
	int nin = numInputs();
	int nout = numOutputs();
	int offset = 0;
	
	if (plugin_){
		int pin = std::min(nin, plugin_->getNumInputs());
		int pout = std::min(nout, plugin_->getNumOutputs());
		for (int chn = 0; chn < pin; ++chn){
			auto input = in(chn);
			auto buf = inBufPtr_[chn];
			for (int i = 0; i < inNumSamples; ++i){
				buf[i] = input[i];
			}
		}
		
		plugin_->process(inBufPtr_, outBufPtr_, inNumSamples);
		
		for (int chn = 0; chn < pout; ++chn){
			auto output = out(chn);
			auto buf = outBufPtr_[chn];
			for (int i = 0; i < inNumSamples; ++i){
				output[i] = buf[i];
			}
		}
		offset = pout;
	} else {
		int n = std::min(nin, nout);
		// bypass
		for (int chn = 0; chn < n; ++chn){
			const float *input = in(chn);
			float *output = out(chn);
			for (int i = 0; i < inNumSamples; ++i){
				output[i] = input[i];
			}
		}
		offset = n;
	}
	// zero remaining outlets
	for (int chn = offset; chn < nout; ++chn){
		float *output = out(chn);
		for (int i = 0; i < inNumSamples; ++i){
			output[i] = 0;
		}
	}
	
}


void VstPlugin_Ctor(VstPlugin* unit){
	new(unit)VstPlugin();
}

void VstPlugin_Dtor(VstPlugin* unit){
	unit->~VstPlugin();
}

// the entry point is called by the host when the plug-in is loaded
PluginLoad(VstPluginUGens) {
    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable
    DefineDtorUnit(VstPlugin);
}
