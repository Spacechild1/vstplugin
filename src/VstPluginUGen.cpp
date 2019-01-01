#include "VstPluginUGen.h"

static InterfaceTable *ft;

void SCLog(const std::string& msg){
	Print(msg.c_str());
}

VstPluginUGen::VstPluginUGen(){
	gui_ = in0(0);
	bypass_ = in0(1);
	numInChannels_ = std::max<int>(0, in0(2));

	Print("VstPluginUGen: nin: %d, nout: %d, gui: %s\n", 
		numInChannels(), numOutChannels(), (gui_ ? "true" : "false"));
	set_calc_function<VstPluginUGen, &VstPluginUGen::next>();
}

VstPluginUGen::~VstPluginUGen(){
	if (buf_) RTFree(mWorld, buf_);
	if (inBufVec_) RTFree(mWorld, inBufVec_);
	if (outBufVec_) RTFree(mWorld, outBufVec_);
	if (paramVec_) RTFree(mWorld, paramVec_);
	if (plugin_) freeVSTPlugin(plugin_);
}

bool VstPluginUGen::check(){
	if (plugin_) {
		return true;
	}
	else {
		Print("VstPluginUGen: no plugin!\n");
		return false;
	}
}

void VstPluginUGen::close() {
	if (plugin_){
		freeVSTPlugin(plugin_);
	}
}

void VstPluginUGen::printInfo() {
	if (check()) {
		Print("%s\n", plugin_->getPluginName().c_str());
		Print("num inputs: %d, num outputs: %d, num parameters: %d\n",
			plugin_->getNumInputs(), plugin_->getNumOutputs(), plugin_->getNumParameters());
		Print("single precision: %s, double precision: %s\n",
			(plugin_->hasPrecision(VSTProcessPrecision::Single) ? "yes" : "no"),
			(plugin_->hasPrecision(VSTProcessPrecision::Double) ? "yes" : "no")
		);
		Print("midi input: %s, midi output: %s\n",
			(plugin_->hasMidiInput() ? "yes" : "no"),
			(plugin_->hasMidiOutput() ? "yes" : "no")
		);
		Print("num programs: %d\n", plugin_->getNumPrograms());
		int nparam = plugin_->getNumParameters();
		for (int i = 0; i < nparam; ++i) {
			Print("param %d: '%s' (%s)\n", i, plugin_->getParameterName(i).c_str(), plugin_->getParameterLabel(i).c_str());
		}
	}
}

void VstPluginUGen::open(const char *path){
	close();
	plugin_ = loadVSTPlugin(makeVSTPluginFilePath(path));
	if (plugin_){
		Print("loaded '%s'!\n", path);
		int blockSize = bufferSize();
		plugin_->setSampleRate(sampleRate());
		plugin_->setBlockSize(blockSize);
		// allocate additional buffers for missing inputs/outputs
		int nin = plugin_->getNumInputs();
		int nout = plugin_->getNumOutputs();
		int inDiff = std::max<int>(0, numInChannels() - nin);
		int outDiff = std::max<int>(0, numOutChannels() - nout);
		// resize buffer (LATER check result of RTRealloc)
		int bufSize = (inDiff + outDiff) * blockSize * sizeof(float);
		buf_ = (float *)RTRealloc(mWorld, buf_, bufSize);
		memset(buf_, 0, bufSize);
		// input buffer array
		inBufVec_ = (const float **)RTRealloc(mWorld, inBufVec_, nin * sizeof(float *));
		for (int i = 0; i < nin; ++i){
			if (i < numInChannels()) {
				inBufVec_[i] = input(i);
			}
			else {
				inBufVec_[i] = &buf_[(i - inDiff) * blockSize];
			}
		}
		// output buffer array
		outBufVec_ = (float **)RTRealloc(mWorld, outBufVec_, nout * sizeof(float *));
		for (int i = 0; i < nout; ++i) {
			if (i < numOutChannels()) {
				outBufVec_[i] = out(i);
			}
			else {
				outBufVec_[i] = &buf_[(i - outDiff + inDiff) * blockSize];
			}
		}
		// allocate arrays for parameter values/states
		int nParams = plugin_->getNumParameters();
		paramVec_ = (Param *)RTRealloc(mWorld, paramVec_, nParams * sizeof(Param));
		for (int i = 0; i < nParams; ++i) {
			paramVec_[i].value = 0;
			paramVec_[i].bus = -1;
		}
	} else {
		Print("couldn't load '%s'!\n", path);
	}
}

void VstPluginUGen::reset() {
	if (check()) {
		plugin_->suspend();
		plugin_->resume();
	}
}

void VstPluginUGen::setParam(int32 index, float value) {
	if (check()) {
		index = clip<int32>(index, 0, plugin_->getNumParameters() - 1);
		plugin_->setParameter(index, value);
		paramVec_[index].value = value;
		paramVec_[index].bus = -1; // invalidate bus num
	}
}

void VstPluginUGen::mapParam(int32 index, int32 bus) {
	if (check()) {
		index = clip<int32>(index, 0, plugin_->getNumParameters() - 1);
		paramVec_[index].bus = std::max<int32>(-1, bus);
	}
}

void VstPluginUGen::next(int inNumSamples){
	int nin = numInChannels();
	int nout = numOutChannels();
	int offset = 0;
	bool bypass = in0(1);
	if (bypass != bypass_) {
		reset();
		bypass_ = bypass;
	}
	
	if (plugin_ && !bypass_){
		int nparam = plugin_->getNumParameters();
		for (int i = 0; i < nparam; ++i) {
			int bus = paramVec_[i].bus;
			if (bus >= 0) {
				float value = readControlBus(bus);
				if (value != paramVec_[i].value) {
					plugin_->setParameter(i, value);
					paramVec_[i].value = value;
				}
			}
		}
		plugin_->process(inBufVec_, outBufVec_, inNumSamples);
		offset = plugin_->getNumOutputs();
	} else {
		int n = std::min(nin, nout);
		// bypass
		for (int chn = 0; chn < n; ++chn){
			const float *_in = input(chn);
			float *_out = out(chn);
			for (int i = 0; i < inNumSamples; ++i){
				_out[i] = _in[i];
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

// helper methods

float VstPluginUGen::readControlBus(int32 num) {
	if (num >= 0 && num < mWorld->mNumControlBusChannels) {
		return mWorld->mControlBus[num];
	}
	else {
		return 0.f;
	}
}

// unit command callbacks

void vst_info(Unit *unit, sc_msg_iter *args) {
	static_cast<VstPluginUGen *>(unit)->printInfo();
}

void vst_open(Unit *unit, sc_msg_iter *args) {
	const char *path = args->gets();
	if (path) {
		static_cast<VstPluginUGen*>(unit)->open(path);
	}
	else {
		Print("vst_open: expecting string argument!\n");
	}
}

void vst_close(Unit *unit, sc_msg_iter *args) {
	static_cast<VstPluginUGen*>(unit)->close();
}

void vst_param_set(Unit *unit, sc_msg_iter *args) {
	int32 index = args->geti();
	float value = args->getf();
	static_cast<VstPluginUGen*>(unit)->setParam(index, value);
}

void vst_param_map(Unit *unit, sc_msg_iter *args) {
	int32 index = args->geti();
	int32 bus = args->getf();
	static_cast<VstPluginUGen*>(unit)->mapParam(index, bus);
}

void vst_test(Unit *unit, sc_msg_iter *args) {
	Print("Unit: test!\n");
	SendTrigger(&unit->mParent->mNode, 13, 0.5);
	float data[] = { 0, 1, 2, 3 };
	SendNodeReply(&unit->mParent->mNode, 14, "/vst_test", 4, data);
}

void VstPluginUGen_Ctor(VstPluginUGen* unit){
	new(unit)VstPluginUGen();
}

void VstPluginUGen_Dtor(VstPluginUGen* unit){
	unit->~VstPluginUGen();
}

#define DefineCmd(x) DefineUnitCmd("VstPluginUGen", "/" #x, vst_##x)

PluginLoad(VstPluginUGen) {
    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable
	DefineDtorCantAliasUnit(VstPluginUGen);
	DefineCmd(info);
	DefineCmd(open);
	DefineCmd(close);
	DefineCmd(param_set);
	DefineCmd(param_map);
}
