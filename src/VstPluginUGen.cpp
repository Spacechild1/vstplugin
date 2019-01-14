#include "VstPluginUGen.h"
#include "Utility.h"

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
    numInChannels_ = std::max<int>(0, in0(0));
    numOutChannels_ = std::max<int>(0, in0(1));

    inBus_ = mWorld->mAudioBus;
    inBusTouched_ = mWorld->mAudioBusTouched;
    outBus_ = mWorld->mAudioBus;
    outBusTouched_ = mWorld->mAudioBusTouched;

    resizeBuffer();

	set_calc_function<VstPluginUGen, &VstPluginUGen::next>();
}

VstPluginUGen::~VstPluginUGen(){
	close();
	if (buf_) RTFree(mWorld, buf_);
	if (inBufVec_) RTFree(mWorld, inBufVec_);
	if (outBufVec_) RTFree(mWorld, outBufVec_);
	if (paramVec_) RTFree(mWorld, paramVec_);
}

bool VstPluginUGen::check(){
	if (plugin_) {
		return true;
	}
	else {
		LOG_WARNING("VstPluginUGen: no plugin!");
		return false;
	}
}

void VstPluginUGen::resizeBuffer(){
    int blockSize = bufferSize();
    int nin = numInChannels_;
    int nout = numOutChannels_;
    if (plugin_){
        nin = std::max<int>(nin, plugin_->getNumInputs());
        nout = std::max<int>(nout, plugin_->getNumOutputs());
    }
    int bufSize = (nin + nout) * blockSize * sizeof(float);
    buf_ = (float *)RTRealloc(mWorld, buf_, bufSize);
    if (!buf_){
        LOG_WARNING("RTRealloc failed!");
        return;
    }
    memset(buf_, 0, bufSize);
    // input buffer array
    inBufVec_ = (float **)RTRealloc(mWorld, inBufVec_, nin * sizeof(float *));
    if (!inBufVec_){
        LOG_WARNING("RTRealloc failed!");
        return;
    }
    for (int i = 0; i < nin; ++i){
        inBufVec_[i] = &buf_[i * blockSize];
    }
    // output buffer array
    outBufVec_ = (float **)RTRealloc(mWorld, outBufVec_, nout * sizeof(float *));
    if (!outBufVec_){
        LOG_WARNING("RTRealloc failed!");
        return;
    }
    for (int i = 0; i < nout; ++i) {
        outBufVec_[i] = &buf_[(i + nin) * blockSize];
    }
}

void VstPluginUGen::close() {
#if VSTTHREADS
        // close editor *before* destroying the window
    if (plugin_) plugin_->closeEditor();
        // destroying the window (if any) might terminate the message loop and already release the plugin
    window_ = nullptr;
        // now join the thread (if any)
    if (thread_.joinable()){
        thread_.join();
		LOG_DEBUG("thread joined");
    }
#endif
        // do we still have a plugin? (e.g. SC editor or !VSTTHREADS)
    if (plugin_){
		    // close editor *before* destroying the window
		plugin_->closeEditor();
        window_ = nullptr;
        freeVSTPlugin(plugin_);
        plugin_ = nullptr;
    }
	LOG_DEBUG("VST plugin closed");
}

void VstPluginUGen::open(const char *path, uint32 flags){
    close();
    bool vstGui = flags & FlagVstGui;
	// initialize GUI backend (if needed)
	if (vstGui) {
    #ifdef __APPLE__
        LOG_WARNING("Warning: VST GUI not supported (yet) on macOS!");
        vstGui = false;
    #else
		static bool initialized = false;
		if (!initialized) {
			VSTWindowFactory::initialize();
			initialized = true;
		}
    #endif
	}
	LOG_DEBUG("try loading plugin");
    plugin_ = tryOpenPlugin(path, vstGui);
    if (plugin_){
        vstGui_ = vstGui;
        paramDisplay_ = flags & FlagParamDisplay;
		LOG_DEBUG("loaded " << path);
		int blockSize = bufferSize();
		plugin_->setSampleRate(sampleRate());
		plugin_->setBlockSize(blockSize);
		if (plugin_->hasPrecision(VSTProcessPrecision::Single)) {
			plugin_->setPrecision(VSTProcessPrecision::Single);
		}
		else {
			LOG_WARNING("VstPluginUGen: plugin '" << plugin_->getPluginName() << "' doesn't support single precision processing - bypassing!");
		}
        resizeBuffer();
		// allocate arrays for parameter values/states
		int nParams = plugin_->getNumParameters();
		paramVec_ = (Param *)RTRealloc(mWorld, paramVec_, nParams * sizeof(Param));
        if (paramVec_){
            for (int i = 0; i < nParams; ++i) {
                paramVec_[i].value = 0;
                paramVec_[i].bus = -1;
            }
        } else {
            LOG_WARNING("RTRealloc failed!");
        }
		sendPluginInfo();
		sendPrograms();
		sendMsg("/vst_open", 1);
		sendParameters(); // after open!
	} else {
		LOG_WARNING("VstPluginUGen: couldn't load " << path);
		sendMsg("/vst_open", 0);
	}
}

IVSTPlugin* VstPluginUGen::tryOpenPlugin(const char *path, bool gui){
#if VSTTHREADS
        // creates a new thread where the plugin is created and the message loop runs
    if (gui){
        std::promise<IVSTPlugin *> promise;
        auto future = promise.get_future();
		LOG_DEBUG("started thread");
        thread_ = std::thread(&VstPluginUGen::threadFunction, this, std::move(promise), path);
        return future.get();
    }
#endif
        // create plugin in main thread
    IVSTPlugin *plugin = loadVSTPlugin(makeVSTPluginFilePath(path));
        // receive events from plugin
    // TODO: plugin->setListener();
#if !VSTTHREADS
        // create and setup GUI window in main thread (if needed)
    if (plugin && plugin->hasEditor() && gui){
        window_ = std::unique_ptr<IVSTWindow>(VSTWindowFactory::create(plugin));
        if (window_){
            window_->setTitle(plugin->getPluginName());
            int left, top, right, bottom;
            plugin->getEditorRect(left, top, right, bottom);
            window_->setGeometry(left, top, right, bottom);
            // don't open the editor on macOS (see VSTWindowCocoa.mm)
#ifndef __APPLE__
            plugin->openEditor(window_->getHandle());
#endif
        }
    }
#endif
    return plugin;
}

#if VSTTHREADS
void VstPluginUGen::threadFunction(std::promise<IVSTPlugin *> promise, const char *path){
    IVSTPlugin *plugin = loadVSTPlugin(makeVSTPluginFilePath(path));
    if (!plugin){
            // signal main thread
        promise.set_value(nullptr);
        return;
    }
        // create GUI window (if needed)
    if (plugin->hasEditor()){
        window_ = std::unique_ptr<IVSTWindow>(VSTWindowFactory::create(plugin));
    }
        // receive events from plugin
    // TODO: plugin->setListener();
        // return plugin to main thread
    promise.set_value(plugin);
        // setup GUI window (if any)
    if (window_){
        window_->setTitle(plugin->getPluginName());
        int left, top, right, bottom;
        plugin->getEditorRect(left, top, right, bottom);
        window_->setGeometry(left, top, right, bottom);

        plugin->openEditor(window_->getHandle());

        window_->run();

        plugin->closeEditor();
            // some plugins expect to released in the same thread where they have been created
        freeVSTPlugin(plugin);
		plugin_ = nullptr;
    }
}
#endif

void VstPluginUGen::showEditor(bool show) {
	if (plugin_ && window_) {
		if (show) {
			window_->bringToTop();
		}
		else {
			window_->hide();
		}
	}
}

void VstPluginUGen::reset() {
	if (check()) {
		plugin_->suspend();
		plugin_->resume();
	}
}

// perform routine
void VstPluginUGen::next(int inNumSamples) {
    if (!(buf_ && inBufVec_ && outBufVec_)) return;
    int nin = numInChannels_;
    int nout = numOutChannels_;
    int32 maxChannel = mWorld->mNumAudioBusChannels;
    int32 bufCounter = mWorld->mBufCounter;
    int32 bufLength = mWorld->mBufLength;
    int32 inBusNum = in0(2);
    int32 outBusNum = in0(3);
    bool bypass = in0(4);
    bool replace = in0(5);

    // check input and output bus
    if (inBusNum != inBusNum_){
        if (inBusNum >= 0 && (inBusNum + nin) < maxChannel){
            inBusNum_ = inBusNum;
            inBus_ = mWorld->mAudioBus + inBusNum * bufLength;
            inBusTouched_ = mWorld->mAudioBusTouched + inBusNum;
        }
    }
    if (outBusNum != outBusNum_){
        if (outBusNum >= 0 && (outBusNum + nout) < maxChannel){
            outBusNum_ = outBusNum;
            outBus_ = mWorld->mAudioBus + outBusNum * bufLength;
            outBusTouched_ = mWorld->mAudioBusTouched + outBusNum;
        }
    }

    // only reset plugin when bypass changed from true to false
    if (!bypass && (bypass != bypass_)) {
        reset();
    }
    bypass_ = bypass;

    // copy from input bus to input buffer
    float *inBus = inBus_;
    for (int i = 0; i < nin; ++i, inBus += bufLength){
        AudioBusGuard<true> guard (this, inBusNum_ + i, maxChannel);
        if (guard.isValid && inBusTouched_[i] == bufCounter){
            Copy(inNumSamples, inBufVec_[i], inBus);
        } else {
            Fill(inNumSamples, inBufVec_[i], 0.f);
        }
    }
    // zero remaining input buffer
    int ninputs = plugin_ ? plugin_->getNumInputs() : 0;
    for (int i = nin; i < ninputs; ++i){
        Fill(inNumSamples, inBufVec_[i], 0.f);
    }

	if (plugin_ && !bypass && plugin_->hasPrecision(VSTProcessPrecision::Single)) {
        // update parameters
        int maxControlChannel = mWorld->mNumControlBusChannels;
        int nparam = plugin_->getNumParameters();
		for (int i = 0; i < nparam; ++i) {
			int bus = paramVec_[i].bus;
			if (bus >= 0) {
                float value = readControlBus(bus, maxControlChannel);
				if (value != paramVec_[i].value) {
					plugin_->setParameter(i, value);
					paramVec_[i].value = value;
				}
			}
		}
        // process
		plugin_->process((const float **)inBufVec_, outBufVec_, inNumSamples);
        // write output buffer to output bus
        float *outBus = outBus_;
        for (int i = 0; i < nout; ++i, outBus += bufLength) {
            AudioBusGuard<false> guard (this, outBusNum_ + i, maxChannel);
            if (guard.isValid) {
                if (!replace && outBusTouched_[i] == bufCounter){
                    Accum(inNumSamples, outBus, outBufVec_[i]);
                } else {
                    Copy(inNumSamples, outBus, outBufVec_[i]);
                    outBusTouched_[i] = bufCounter;
                }
            }
        }
	}
    else {
        // bypass (write input buffer to output bus)
        int n = std::min(nin, nout);
        float *outBus = outBus_;
        for (int i = 0; i < n; ++i, outBus += bufLength) {
            AudioBusGuard<false> guard (this, outBusNum_ + i, maxChannel);
            if (guard.isValid) {
                if (!replace && outBusTouched_[i] == bufCounter){
                    Accum(inNumSamples, outBus, inBufVec_[i]);
                } else {
                    Copy(inNumSamples, outBus, inBufVec_[i]);
                    outBusTouched_[i] = bufCounter;
                }
            }
        }
    }
}

void VstPluginUGen::setParam(int32 index, float value) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumParameters()) {
			plugin_->setParameter(index, value);
			paramVec_[index].value = value;
			paramVec_[index].bus = -1; // invalidate bus num
			if (paramDisplay_) {
				const int maxSize = 64;
				float buf[maxSize];
				buf[0] = index;
				int len = string2floatArray(plugin_->getParameterDisplay(index), buf + 1, maxSize - 1);
				sendMsg("/vst_param_display", len + 1, buf);
			}
		}
		else {
			LOG_WARNING("VstPluginUGen: parameter index " << index << " out of range!");
		}
	}
}

void VstPluginUGen::mapParam(int32 index, int32 bus) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumParameters()) {
			paramVec_[index].bus = std::max<int32>(-1, bus);
		}
		else {
			LOG_WARNING("VstPluginUGen: parameter index " << index << " out of range!");
		}
	}
}

// program/bank
void VstPluginUGen::setProgram(int32 index) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumPrograms()) {
			plugin_->setProgram(index);
			sendMsg("/vst_program", index);
			sendParameters();
		}
		else {
			LOG_WARNING("VstPluginUGen: program number " << index << " out of range!");
		}
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
			LOG_WARNING("VstPluginUGen: couldn't read program file '" << path << "'");
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
			LOG_WARNING("VstPluginUGen: couldn't read program data");
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
				LOG_WARNING("program data size (" << len << ") probably too large for UDP packet");
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
				LOG_ERROR("RTAlloc failed!");
			}
		}
		else {
			LOG_WARNING("VstPluginUGen: couldn't write program data");
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
			LOG_WARNING("VstPluginUGen: couldn't read bank file '" << path << "'");
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
			LOG_WARNING("VstPluginUGen: couldn't read bank data");
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
				LOG_WARNING("bank data size (" << len << ") probably too large for UDP packet");
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
				LOG_ERROR("RTAlloc failed!");
			}
		}
		else {
			LOG_WARNING("VstPluginUGen: couldn't write bank data");
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

// helper methods

float VstPluginUGen::readControlBus(int32 num, int32 maxChannel) {
    if (num >= 0 && num < maxChannel) {
		return mWorld->mControlBus[num];
	}
	else {
		return 0.f;
	}
}

void VstPluginUGen::sendPluginInfo() {
    const int maxSize = 64;
    float buf[maxSize];

    int nameLen = string2floatArray(plugin_->getPluginName(), buf, maxSize);
    sendMsg("/vst_name", nameLen, buf);
	sendMsg("/vst_editor", (float)plugin_->hasEditor());
	sendMsg("/vst_nin", plugin_->getNumInputs());
	sendMsg("/vst_nout", plugin_->getNumOutputs());
	sendMsg("/vst_midiin", plugin_->hasMidiInput());
	sendMsg("/vst_midiout", plugin_->hasMidiOutput());
	sendMsg("/vst_nprograms", plugin_->getNumPrograms());
	// send parameter info
	int nparams = plugin_->getNumParameters();
    sendMsg("/vst_nparams", nparams);
	// send parameter names
	for (int i = 0; i < nparams; ++i) {
		// msg format: index, len, characters...
		buf[0] = i;
        int len = string2floatArray(plugin_->getParameterName(i), buf + 1, maxSize - 1);
		sendMsg("/vst_param_name", len + 1, buf);
	}
	// send parameter labels
	if (paramDisplay_) {
		for (int i = 0; i < nparams; ++i) {
			// msg format: index, len, characters...
			buf[0] = i;
			int len = string2floatArray(plugin_->getParameterLabel(i), buf + 1, maxSize - 1);
			sendMsg("/vst_param_label", len + 1, buf);
		}
	}
}

void VstPluginUGen::sendPrograms() {
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
    const int maxSize = 64;
    float buf[maxSize];
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
    int len = string2floatArray(name, buf + 1, maxSize - 1);
	sendMsg("/vst_program_name", len + 1, buf);
	return changed;
}

void VstPluginUGen::sendParameters() {
	const int maxSize = 64;
	float buf[maxSize];
	int nparam = plugin_->getNumParameters();
	for (int i = 0; i < nparam; ++i) {
		buf[0] = i;
		buf[1] = plugin_->getParameter(i);
		sendMsg("/vst_param", 2, buf);
		if (paramDisplay_) {
			int len = string2floatArray(plugin_->getParameterDisplay(i), buf + 1, maxSize - 1);
			sendMsg("/vst_param_display", len + 1, buf);
		}
	}
}

void VstPluginUGen::sendMsg(const char *cmd, float f) {
	// LOG_DEBUG("sending msg: " << cmd);
	SendNodeReply(&mParent->mNode, mParentIndex, cmd, 1, &f);
}

void VstPluginUGen::sendMsg(const char *cmd, int n, const float *data) {
	// LOG_DEBUG("sending msg: " << cmd);
	SendNodeReply(&mParent->mNode, mParentIndex, cmd, n, data);
}

// unit command callbacks

void vst_open(Unit *unit, sc_msg_iter *args) {
    uint32 flags = args->geti();
	const char *path = args->gets();
	if (path) {
        static_cast<VstPluginUGen*>(unit)->open(path, flags);
	}
	else {
		LOG_WARNING("vst_open: expecting string argument!");
	}
}

void vst_close(Unit *unit, sc_msg_iter *args) {
	static_cast<VstPluginUGen*>(unit)->close();
}

void vst_reset(Unit *unit, sc_msg_iter *args) {
	static_cast<VstPluginUGen*>(unit)->reset();
}

void vst_vis(Unit *unit, sc_msg_iter *args) {
	bool show = args->geti();
	static_cast<VstPluginUGen*>(unit)->showEditor(show);
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
		LOG_WARNING("vst_program_name: expecting string argument!");
	}
}

void vst_program_read(Unit *unit, sc_msg_iter *args) {
	const char *path = args->gets();
	if (path) {
		static_cast<VstPluginUGen*>(unit)->readProgram(path);
	}
	else {
		LOG_WARNING("vst_program_read: expecting string argument!");
	}
}

void vst_program_write(Unit *unit, sc_msg_iter *args) {
	const char *path = args->gets();
	if (path) {
		static_cast<VstPluginUGen*>(unit)->writeProgram(path);
	}
	else {
		LOG_WARNING("vst_program_write: expecting string argument!");
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
			LOG_ERROR("vst_program_data_set: RTAlloc failed!");
		}
	}
	else {
		LOG_WARNING("vst_program_data_set: no data!");
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
		LOG_WARNING("vst_bank_read: expecting string argument!");
	}
}

void vst_bank_write(Unit *unit, sc_msg_iter *args) {
	const char *path = args->gets();
	if (path) {
		static_cast<VstPluginUGen*>(unit)->writeBank(path);
	}
	else {
		LOG_WARNING("vst_bank_write: expecting string argument!");
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
			LOG_ERROR("vst_bank_data_set: RTAlloc failed!");
		}
	}
	else {
		LOG_WARNING("vst_bank_data_set: no data!");
	}
}

void vst_bank_data_get(Unit *unit, sc_msg_iter *args) {
	static_cast<VstPluginUGen*>(unit)->getBankData();
}


void vst_midi_msg(Unit *unit, sc_msg_iter *args) {
	char data[4];
	int32 len = args->getbsize();
	if (len > 4) {
		LOG_WARNING("vst_midi_msg: midi message too long (" << len << " bytes)");
	}
	args->getb(data, len);
	static_cast<VstPluginUGen*>(unit)->sendMidiMsg(data[0], data[1], data[2]);
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
			LOG_ERROR("vst_midi_sysex: RTAlloc failed!");
		}
	}
	else {
		LOG_WARNING("vst_midi_sysex: no data!");
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

void vst_poll(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
    VSTWindowFactory::mainLoopPoll();
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
	DefineCmd(reset);
	DefineCmd(vis);
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

    DefinePlugInCmd("vst_poll", vst_poll, 0);
}
