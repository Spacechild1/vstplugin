#include "vsthost.h"

#include <algorithm>
#include <iostream>

// VST host

// public

VSTHost::VSTHost(int nin, int nout) {
	input_.resize(nin);
	output_.resize(nout);
}
	
VSTHost::~VSTHost(){
	closePlugin();
}

void VSTHost::perform(int n){
	int nin = getNumHostInputs();
	int nout = getNumHostOutputs();
	/* copy input to input buffer */
	auto bufin = inBuffer_.begin();
	for (int i = 0; i < nin; ++i){
		bufin = std::copy(input_[i], input_[i]+n, bufin);
	}
	// zero remaining input buffer
	std::fill(bufin, inBuffer_.end(), 0);
	
	if (plugin_ && !bypass_){ // process
		/* clear output buffer */
		std::fill(outBuffer_.begin(), outBuffer_.end(), 0);
		/* process audio */
		process(inBufferVec_.data(), outBufferVec_.data(), n);
		/* copy output buffer to output */
		auto bufout = outBuffer_.begin();
		for (int i = 0; i < nout; ++i){
			std::copy(bufout, bufout+n, output_[i]);
			bufout += n;
		}
	} else { // bypass
		/* copy input buffer to output */
		bufin = inBuffer_.begin();
		int i = 0;
		for (; i < nout && i < nin; ++i){
			std::copy(bufin, bufin+n, output_[i]);
			bufin += n;
		}
		for (; i < nout; ++i){ // zero remaining outputs
			std::fill(output_[i], output_[i]+n, 0);
		}
	}
}

void VSTHost::setSampleRate(float sr){
	if (sr > 0){
		samplerate_ = sr;
		if (plugin_){
			dispatch(effSetSampleRate, 0, 0, NULL, samplerate_);
		}
	} else {
		post("VSTHost: bad sample rate %f!", sr);
	}
}

void VSTHost::setBlockSize(int n){
	if (n > 0 && !(n & (n-1))){ // check if power of 2
		blocksize_ = n;
		if (plugin_){
			dispatch(effSetBlockSize, 0, blocksize_, NULL, 0.f);
		}
		updateBuffers();
	} else {
		post("VSTHost: bad block size %d!", n);
	}
}

void VSTHost::setInputBuffer(int chn, float* buf){
	if (chn >= 0 && chn < getNumHostInputs()){
		input_[chn] = buf;
	} else {
		post("VSTHost::setInputBuffer: channel out of range!");
	}
}

void VSTHost::setOutputBuffer(int chn, float* buf){
	if (chn >= 0 && chn < getNumHostOutputs()){
		output_[chn] = buf;
	} else {
		post("VSTHost::setOutputBuffer: channel out of range!");
	}
}

int VSTHost::getNumHostInputs() const {
	return input_.size();
}

int VSTHost::getNumHostOutputs() const {
	return output_.size();
}

int VSTHost::getNumPluginInputs() const {
	if (plugin_){
		return plugin_->numInputs;
	} else {
		return -1;
	}
}

int VSTHost::getNumPluginOutputs() const {
	if (plugin_){
		return plugin_->numOutputs;
	} else {
		return -1;
	}
}

void VSTHost::resume(){
	if (plugin_){
		dispatch(effMainsChanged, 0, 1, NULL, 0.f);
	}
}

void VSTHost::pause(){
	if (plugin_){
		dispatch(effMainsChanged, 0, 0, NULL, 0.f);
	}
}

void VSTHost::setBypass(bool bypass){
	bypass_ = bypass;
}

void VSTHost::openPlugin(const std::string& path){
	closePlugin();
	AEffect *plugin = nullptr;
	HMODULE handle = LoadLibraryA(path.c_str());
	if (handle == NULL){
		post("couldn't open %s", path.c_str());
		return;
	}
	vstPluginFuncPtr mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "VSTPluginMain"));
	if (mainEntryPoint == NULL){
		mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "main"));
	}
	if (mainEntryPoint == NULL){
		post("couldn't find entry point in VST plugin");
		return;
	}
	plugin = mainEntryPoint(hostCallback);
	if (plugin == NULL){
		post("couldn't initialize plugin");
		return;
	}
	if (plugin->magic != kEffectMagic){
		post("bad magic number!");
		return;
	}
	plugin_ = plugin;
	
	post("successfully loaded plugin");
	
	dispatch(effOpen, 0, 0, NULL, 0.f);
	
	setupParameters();
	
	setSampleRate(samplerate_);
	setBlockSize(blocksize_);
	
	resume();
}

void VSTHost::closePlugin(){
	if (plugin_){
		dispatch(effClose, 0, 0, NULL, 0.f);
		plugin_ = nullptr;
		paramProps_.clear();
	}
}

bool VSTHost::hasPlugin() const {
	return plugin_;
}

void VSTHost::showEditor(){
	dispatch(effEditOpen, 0, 0, NULL, 0.f);
}

void VSTHost::hideEditor(){
	dispatch(effMainsChanged, 0, 0, NULL, 0.f);
}

void VSTHost::setParameter(int index, float value){
	if (plugin_ && plugin_->setParameter){
		(plugin_->setParameter)(plugin_, index, value);
	} else {
		error("can't set parameter - no plugin!");
	}
}

float VSTHost::getParameter(int index) const {
	if (plugin_ && plugin_->getParameter){
		return (plugin_->getParameter)(plugin_, index);
	} else {
		error("can't get parameter - no plugin!");
		return 0;
	}
}

int VSTHost::getNumParameters() const {
	if (plugin_){
		return plugin_->numParams;
	} else {
		return -1;
	}
}

std::string VSTHost::getParameterName(int index) const {
	char buf[kVstMaxParamStrLen];
	buf[0] = 0;
	dispatch(effGetParamName, index, 0, buf, 0.f);
	return std::string(buf);
}

void VSTHost::setProgram(int program){
	if (program >= 0 && program < getNumPrograms()){
		dispatch(effSetProgram, 0, program, NULL, 0.f);
	} else {
		error("program number out of range!");
	}
}

int VSTHost::getProgram(){
	if (plugin_){
		return dispatch(effGetProgram, 0, 0, NULL, 0.f);
	} else {
		return -1;
	}
}

int VSTHost::getNumPrograms() const {
	if (plugin_){
		return plugin_->numPrograms;
	} else {
		return -1;
	}
}

std::string VSTHost::getProgramName() const {
	char buf[kVstMaxProgNameLen];
	buf[0] = 0;
	dispatch(effGetProgramName, 0, 0, buf, 0.f);
	return std::string(buf);
}

void VSTHost::setProgramName(const std::string& name){
	dispatch(effSetProgramName, 0, 0, (void*)name.c_str(), 0.f);
}

int VSTHost::getVstVersion() const {
	if (plugin_){
		return plugin_->version;
	} else {
		return -1;
	}
}

// private

VstIntPtr VSTHost::dispatch(VstInt32 opCode,
	VstInt32 index, VstInt32 value, void *ptr, float opt) const {
	if (plugin_ && plugin_->dispatcher){
		return (plugin_->dispatcher)(plugin_, opCode, index, value, ptr, opt);
	} else {
		error("can't dispatch - no plugin!");
		return 0;
	}
}

void VSTHost::process(float **inputs,
	float **outputs, VstInt32 sampleFrames){
	if (plugin_ && plugin_->processReplacing){
		(plugin_->processReplacing)(plugin_, inputs, outputs, sampleFrames);
	} else {
		error("can't set parameter - no plugin!");
	}
}

void VSTHost::updateBuffers(){
	/* resize buffers */
	const int n = blocksize_;
	int nin = input_.size();
	int nout = output_.size();
	if (plugin_){
		if (nin < plugin_->numInputs) nin = plugin_->numInputs;
		if (nout < plugin_->numOutputs) nin = plugin_->numOutputs;
	}
	inBuffer_.resize(nin * n);
	outBuffer_.resize(nout * n);
	inBufferVec_.resize(nin);
	outBufferVec_.resize(nout);
	for (int i = 0; i < nin; ++i){
		inBufferVec_[i] = &inBuffer_[i * n];
	}
	for (int i = 0; i < nout; ++i){
		outBufferVec_[i] = &outBuffer_[i * n];
	}
}

// Main host callback
VstIntPtr VSTCALLBACK VSTHost::hostCallback(AEffect *plugin, VstInt32 opcode, 
	VstInt32 index, VstInt32 value, void *ptr, float opt){
	switch(opcode) {
	case audioMasterVersion:
	  return 2400;
	case audioMasterIdle:
	  plugin->dispatcher(plugin, effEditIdle, 0, 0, 0, 0);
	  break;
	// Handle other opcodes here... there will be lots of them
	default:
	  break;
	}
	post("plugin requested opcode %d", opcode);
	return 0; // ?
}

void VSTHost::setupParameters(){
	// TODO
#if 0
	int numParams = plugin_->numParams;
	post("num params: %d", numParams);
	char buf[kVstMaxParamStrLen];
	buf[0] = 0;
	for (int i = 0; i < numParams; ++i){
		dispatch(effGetParamName, i, 0, buf, 0.f);
		post("param %d: %s", i, buf);
		VstParameterProperties props;
		memset(&props, 0, sizeof(props));
		if (dispatch(effGetParameterProperties, i, 0, &props, 0.f)){
			paramProps_.emplace(i, props);
			post("stepFloat: %f, smallStepFloat: %f, largeStepFloat: %f, label: %s, flags: %d, "
				"minInteger: %d, maxInteger %d, stepInteger %d, largeStepInteger %d",
				props.stepFloat, props.smallStepFloat, props.largeStepFloat, props.label, props.flags,
				props.minInteger, props.maxInteger, props.stepInteger, props.largeStepInteger);
		}
	}
#endif
}
