#include "VstPluginUGen.h"

static InterfaceTable *ft;

void SCLog(const std::string& msg){
	Print(msg.c_str());
}

int string2floatArray(const std::string& src, float *dest, int maxSize) {
	int len = std::min<int>(src.size(), maxSize);
	for (int i = 0; i < len; ++i) {
		dest[i] = src[i];
	}
	return len;
}

VstPluginUGen::VstPluginUGen(){
	gui_ = in0(0);
	bypass_ = in0(1);
	numInChannels_ = std::max<int>(0, in0(2));

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

void VstPluginUGen::open(const char *path){
	close();
	plugin_ = loadVSTPlugin(makeVSTPluginFilePath(path));
	if (plugin_){
		// Print("loaded '%s'!\n", path);
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
		sendPluginInfo();
		sendPrograms();
		sendParameters();
		sendMsg("/vst_open", 1);
	} else {
		Print("couldn't load '%s'!\n", path);
		sendMsg("/vst_open", 0);
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

// program/bank
void VstPluginUGen::setProgram(int32 index) {
	if (check()) {
		index = clip<int32>(index, 0, plugin_->getNumPrograms());
		plugin_->setProgram(index);
		sendMsg("/vst_program", index);
		sendParameters();
	}
}
void VstPluginUGen::setProgramName(const char *name) {
	if (check()) {
		plugin_->setProgramName(name);
		sendProgram(plugin_->getProgram());
	}
}
void VstPluginUGen::readProgram(const char *path) {
	if (check()) {
		if (plugin_->readProgramFile(path)) {
			sendParameters();
		}
		else {
			Print("couldn't read program file '%s'\n", path);
		}
	}
}
void VstPluginUGen::writeProgram(const char *path) {
	if (check()) {
		plugin_->writeProgramFile(path);
	}
}
void VstPluginUGen::setProgramData(const char *data, int32 n) {
	if (check()) {
		if (plugin_->readProgramData(data, n)) {
			sendParameters();
		}
		else {
			Print("couldn't read program data\n");
		}
	}
}
void VstPluginUGen::getProgramData() {
	if (check()) {
		std::string data;
		plugin_->writeProgramData(data);
		int len = data.size();
		if (len) {
			if ((len * sizeof(float)) > 8000) {
				Print("program data size (%d) probably too large for UDP packet\n", len);
			}
			float *buf = (float *)RTAlloc(mWorld, sizeof(float) * len);
			if (buf) {
				for (int i = 0; i < len; ++i) {
					buf[i] = data[i];
				}
				sendMsg("/vst_program_data", len, buf);
				RTFree(mWorld, buf);
			}
			else {
				Print("RTAlloc failed!\n");
			}
		}
		else {
			Print("couldn't write program data\n");
		}
	}
}
void VstPluginUGen::readBank(const char *path) {
	if (check()) {
		if (plugin_->readBankFile(path)) {
			sendPrograms();
			sendParameters();
			sendMsg("/vst_program", plugin_->getProgram());
		}
		else {
			Print("couldn't read bank file '%s'\n", path);
		}
	}
}
void VstPluginUGen::writeBank(const char *path) {
	if (check()) {
		plugin_->writeBankFile(path);
	}
}
void VstPluginUGen::setBankData(const char *data, int32 n) {
	if (check()) {
		if (plugin_->readBankData(data, n)) {
			sendPrograms();
			sendParameters();
			sendMsg("/vst_program", plugin_->getProgram());
		}
		else {
			Print("couldn't read bank data\n");
		}
	}
}
void VstPluginUGen::getBankData() {
	if (check()) {
		std::string data;
		plugin_->writeBankData(data);
		int len = data.size();
		if (len) {
			if ((len * sizeof(float)) > 8000) {
				Print("bank data size (%d) probably too large for UDP packet\n", len);
			}
			float *buf = (float *)RTAlloc(mWorld, sizeof(float) * len);
			if (buf) {
				for (int i = 0; i < len; ++i) {
					buf[i] = data[i];
				}
				sendMsg("/vst_bank_data", len, buf);
				RTFree(mWorld, buf);
			}
			else {
				Print("RTAlloc failed!\n");
			}
		}
		else {
			Print("couldn't write bank data\n");
		}
	}
}
// midi
void VstPluginUGen::sendMidiMsg(int32 status, int32 data1, int32 data2) {
	if (check()) {
		plugin_->sendMidiEvent(VSTMidiEvent(status, data1, data2));
	}
}
void VstPluginUGen::sendSysexMsg(const char *data, int32 n) {
	if (check()) {
		plugin_->sendSysexEvent(VSTSysexEvent(data, n));
	}
}
// transport
void VstPluginUGen::setTempo(float bpm) {
	if (check()) {
		plugin_->setTempoBPM(bpm);
	}
}
void VstPluginUGen::setTimeSig(int32 num, int32 denom) {
	if (check()) {
		plugin_->setTimeSignature(num, denom);
	}
}
void VstPluginUGen::setTransportPlaying(bool play) {
	if (check()) {
		plugin_->setTransportPlaying(play);
	}
}
void VstPluginUGen::setTransportPos(float pos) {
	if (check()) {
		plugin_->setTransportPosition(pos);
	}
}
void VstPluginUGen::getTransportPos() {
	if (check()) {
		float f = plugin_->getTransportPosition();
		sendMsg("/vst_transport", f);
	}
}

// perform routine
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

void VstPluginUGen::sendPluginInfo() {
	float nameBuf[256];
	int nameLen = string2floatArray(plugin_->getPluginName(), nameBuf, 256);
	sendMsg("/vst_name", nameLen, nameBuf);
	sendMsg("/vst_nin", plugin_->getNumInputs());
	sendMsg("/vst_nout", plugin_->getNumOutputs());
	sendMsg("/vst_midiin", plugin_->hasMidiInput());
	sendMsg("/vst_midiout", plugin_->hasMidiOutput());
	sendMsg("/vst_nprograms", plugin_->getNumPrograms());
	// send parameter info
	int nparams = plugin_->getNumParameters();
	sendMsg("/vst_nparams", nparams);
	const int maxSize = 256;
	float buf[maxSize + 1];
	for (int i = 0; i < nparams; ++i) {
		// msg format: index, len, characters...
		buf[0] = i;
		int len = string2floatArray(plugin_->getParameterName(i), buf + 1, maxSize);
		sendMsg("/vst_param_name", len+1, buf);
	}
}

void VstPluginUGen::sendPrograms() {
	const int maxSize = 256;
	float buf[maxSize+1];
	int current = plugin_->getProgram();
	bool changed = false;
	int nprograms = plugin_->getNumPrograms();
	for (int i = 0; i < nprograms; ++i) {
		changed = sendProgram(i);
	}
	if (changed) {
		plugin_->setProgram(current);
	}
}

bool VstPluginUGen::sendProgram(int32 num) {
	const int maxSize = 256;
	float buf[maxSize + 1];
	bool changed = false;
	auto name = plugin_->getProgramNameIndexed(num);
	// some old plugins don't support indexed program name lookup
	if (name.empty()) {
		plugin_->setProgram(num);
		name = plugin_->getProgramName();
		changed = true;
	}
	// msg format: index, len, characters...
	buf[0] = num;
	int len = string2floatArray(name, buf + 1, maxSize);
	sendMsg("/vst_program_name", len + 1, buf);
	return changed;
}

void VstPluginUGen::sendParameters() {
	int nparam = plugin_->getNumParameters();
	for (int i = 0; i < nparam; ++i) {
		float buf[2] = { (float)i, plugin_->getParameter(i) };
		sendMsg("/vst_param", 2, buf);
	}
}

void VstPluginUGen::sendMsg(const char *cmd, float f) {
	SendNodeReply(&mParent->mNode, mParentIndex, cmd, 1, &f);
}

void VstPluginUGen::sendMsg(const char *cmd, int n, const float *data) {
	SendNodeReply(&mParent->mNode, mParentIndex, cmd, n, data);
}

// unit command callbacks

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

void vst_program_set(Unit *unit, sc_msg_iter *args) {
	int32 index = args->geti();
	static_cast<VstPluginUGen*>(unit)->setProgram(index);
}

void vst_program_name(Unit *unit, sc_msg_iter *args) {
	const char *name = args->gets();
	if (name) {
		static_cast<VstPluginUGen*>(unit)->setProgramName(name);
	}
	else {
		Print("vst_program_name: expecting string argument!\n");
	}
}

void vst_program_read(Unit *unit, sc_msg_iter *args) {
	const char *path = args->gets();
	if (path) {
		static_cast<VstPluginUGen*>(unit)->readProgram(path);
	}
	else {
		Print("vst_program_read: expecting string argument!\n");
	}
}

void vst_program_write(Unit *unit, sc_msg_iter *args) {
	const char *path = args->gets();
	if (path) {
		static_cast<VstPluginUGen*>(unit)->writeProgram(path);
	}
	else {
		Print("vst_program_write: expecting string argument!\n");
	}
}

void vst_program_data_set(Unit *unit, sc_msg_iter *args) {
	int len = args->getbsize();
	if (len > 0) {
		// LATER avoid unnecessary copying
		char *buf = (char *)RTAlloc(unit->mWorld, len);
		if (buf) {
			args->getb(buf, len);
			static_cast<VstPluginUGen*>(unit)->setProgramData(buf, len);
			RTFree(unit->mWorld, buf);
		}
		else {
			Print("vst_program_data_set: RTAlloc failed!\n");
		}
	}
	else {
		Print("vst_program_data_set: no data!\n");
	}
}

void vst_program_data_get(Unit *unit, sc_msg_iter *args) {
	static_cast<VstPluginUGen*>(unit)->getProgramData();
}

void vst_bank_read(Unit *unit, sc_msg_iter *args) {
	const char *path = args->gets();
	if (path) {
		static_cast<VstPluginUGen*>(unit)->readBank(path);
	}
	else {
		Print("vst_bank_read: expecting string argument!\n");
	}
}

void vst_bank_write(Unit *unit, sc_msg_iter *args) {
	const char *path = args->gets();
	if (path) {
		static_cast<VstPluginUGen*>(unit)->writeBank(path);
	}
	else {
		Print("vst_bank_write: expecting string argument!\n");
	}
}

void vst_bank_data_set(Unit *unit, sc_msg_iter *args) {
	int len = args->getbsize();
	if (len > 0) {
		// LATER avoid unnecessary copying
		char *buf = (char *)RTAlloc(unit->mWorld, len);
		if (buf) {
			args->getb(buf, len);
			static_cast<VstPluginUGen*>(unit)->setBankData(buf, len);
			RTFree(unit->mWorld, buf);
		}
		else {
			Print("vst_bank_data_set: RTAlloc failed!\n");
		}
	}
	else {
		Print("vst_bank_data_set: no data!\n");
	}
}

void vst_bank_data_get(Unit *unit, sc_msg_iter *args) {
	static_cast<VstPluginUGen*>(unit)->getBankData();
}


void vst_midi_msg(Unit *unit, sc_msg_iter *args) {
	int32 status = args->geti();
	int32 data1 = args->geti();
	int32 data2 = args->geti();
	static_cast<VstPluginUGen*>(unit)->sendMidiMsg(status, data1, data2);
}

void vst_midi_sysex(Unit *unit, sc_msg_iter *args) {
	int len = args->getbsize();
	if (len > 0) {
		// LATER avoid unnecessary copying
		char *buf = (char *)RTAlloc(unit->mWorld, len);
		if (buf) {
			args->getb(buf, len);
			static_cast<VstPluginUGen*>(unit)->sendSysexMsg(buf, len);
			RTFree(unit->mWorld, buf);
		}
		else {
			Print("vst_midi_sysex: RTAlloc failed!\n");
		}
	}
	else {
		Print("vst_midi_sysex: no data!\n");
	}
}

void vst_tempo(Unit *unit, sc_msg_iter *args) {
	float bpm = args->getf();
	static_cast<VstPluginUGen*>(unit)->setTempo(bpm);
}

void vst_time_sig(Unit *unit, sc_msg_iter *args) {
	int32 num = args->geti();
	int32 denom = args->geti();
	static_cast<VstPluginUGen*>(unit)->setTimeSig(num, denom);
}

void vst_transport_play(Unit *unit, sc_msg_iter *args) {
	int play = args->geti();
	static_cast<VstPluginUGen*>(unit)->setTransportPlaying(play);
}

void vst_transport_set(Unit *unit, sc_msg_iter *args) {
	float pos = args->getf();
	static_cast<VstPluginUGen*>(unit)->setTransportPos(pos);
}

void vst_transport_get(Unit *unit, sc_msg_iter *args) {
	static_cast<VstPluginUGen*>(unit)->getTransportPos();
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
	DefineCmd(open);
	DefineCmd(close);
	DefineCmd(param_set);
	DefineCmd(param_map);
	DefineCmd(program_set);
	DefineCmd(program_name);
	DefineCmd(program_read);
	DefineCmd(program_write);
	DefineCmd(program_data_set);
	DefineCmd(program_data_get);
	DefineCmd(bank_read);
	DefineCmd(bank_write);
	DefineCmd(bank_data_set);
	DefineCmd(bank_data_get);
	DefineCmd(midi_msg);
	DefineCmd(midi_sysex);
	DefineCmd(tempo);
	DefineCmd(time_sig);
	DefineCmd(transport_play);
	DefineCmd(transport_set);
	DefineCmd(transport_get);
}
