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

// VstPluginListener

VstPluginListener::VstPluginListener(VstPlugin &owner)
	: owner_(&owner) {}

void VstPluginListener::parameterAutomated(int index, float value) {
#if VSTTHREADS
	std::lock_guard<std::mutex> guard(owner_->mutex_);
	auto& queue = owner_->paramQueue_;
	// resize queue if necessary
	if (queue.size >= queue.capacity) {
		// start with initial capacity of 8, then double it whenever it needs to be increased
		auto newCapacity = queue.capacity > 0 ? queue.capacity * 2 : 8;
		auto result = (VstPlugin::ParamAutomated *)RTRealloc(owner_->mWorld, queue.data, newCapacity);
		if (result) {
			queue.data = result;
			queue.capacity = newCapacity;
			LOG_DEBUG("ParamQueue new capacity: " << newCapacity);
		}
		else {
			LOG_ERROR("RTRealloc failed!");
			return;
		}
	}
	// append item
	auto& newItem = queue.data[queue.size];
	newItem.index = index;
	newItem.value = value;
	queue.size++;
#else
	owner_->parameterAutomated(index, value);
#endif
}

void VstPluginListener::midiEvent(const VSTMidiEvent& midi) {
#if VSTTHREADS
	// check if we're on the realtime thread, otherwise ignore it
	if (std::this_thread::get_id() == owner_->threadID_) {
#else
	{
#endif
		owner_->midiEvent(midi);
	}
}

void VstPluginListener::sysexEvent(const VSTSysexEvent& sysex) {
#if VSTTHREADS
	// check if we're on the realtime thread, otherwise ignore it
	if (std::this_thread::get_id() == owner_->threadID_) {
#else
	{
#endif
		owner_->sysexEvent(sysex);
	}
}

// VstPlugin

VstPlugin::VstPlugin(){
#if VSTTHREADS
	threadID_ = std::this_thread::get_id();
	// LOG_DEBUG("thread ID constructor: " << threadID_);
#endif
	listener_ = std::make_unique<VstPluginListener>(*this);

	numInChannels_ = in0(1);
	numOutChannels_ = numOutputs();
	parameterControlOnset_ = inChannelOnset_ + numInChannels_;
	numParameterControls_ = (int)(numInputs() - parameterControlOnset_) / 2;
	// LOG_DEBUG("num in: " << numInChannels_ << ", num out: " << numOutChannels_ << ", num controls: " << numParameterControls_);
    resizeBuffer();

	set_calc_function<VstPlugin, &VstPlugin::next>();
}

VstPlugin::~VstPlugin(){
	close();
	if (buf_) RTFree(mWorld, buf_);
	if (inBufVec_) RTFree(mWorld, inBufVec_);
	if (outBufVec_) RTFree(mWorld, outBufVec_);
	if (paramStates_) RTFree(mWorld, paramStates_);
	if (paramQueue_.data) RTFree(mWorld, paramQueue_.data);
}

IVSTPlugin *VstPlugin::plugin() {
	return plugin_;
}

bool VstPlugin::check(){
	if (plugin_) {
		return true;
	}
	else {
		LOG_WARNING("VstPlugin: no plugin!");
		return false;
	}
}

bool VstPlugin::valid() {
	if (magic_ == MagicNumber) {
		return true;
	}
	else {
		LOG_WARNING("VstPlugin (" << mParent->mNode.mID << ", " << mParentIndex << ") not ready!");
		return false;
	}
}

void VstPlugin::resizeBuffer(){
    int blockSize = bufferSize();
    int nin = numInChannels_;
    int nout = numOutChannels_;
	bool fail = false;
    if (plugin_){
        nin = std::max<int>(nin, plugin_->getNumInputs());
        nout = std::max<int>(nout, plugin_->getNumOutputs());
    }
	// buffer
	{
		int bufSize = (nin + nout) * blockSize * sizeof(float);
		auto result = (float *)RTRealloc(mWorld, buf_, bufSize);
		if (result) {
			buf_ = result;
			memset(buf_, 0, bufSize);
		}
		else {
			fail = true;
		}
	}
    // input buffer array
	{
		auto result = (const float **)RTRealloc(mWorld, inBufVec_, nin * sizeof(float *));
		if (result) {
			inBufVec_ = result;
			for (int i = 0; i < nin; ++i) {
				inBufVec_[i] = &buf_[i * blockSize];
			}
		}
		else {
			fail = true;
		}
	}
    // output buffer array
	{
		auto result = (float **)RTRealloc(mWorld, outBufVec_, nout * sizeof(float *));
		if (result) {
			outBufVec_ = result;
			for (int i = 0; i < nout; ++i) {
				outBufVec_[i] = &buf_[(i + nin) * blockSize];
			}
		}
		else {
			fail = true;
		}
	}
	if (fail) {
		LOG_ERROR("RTRealloc failed!");
		RTFree(mWorld, buf_);
		RTFree(mWorld, inBufVec_);
		RTFree(mWorld, outBufVec_);
		buf_ = nullptr; inBufVec_ = nullptr; outBufVec_ = nullptr;
	}
}

void VstPlugin::close() {
#if VSTTHREADS
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
        window_ = nullptr;
        freeVSTPlugin(plugin_);
        plugin_ = nullptr;
    }
	LOG_DEBUG("VST plugin closed");
}

void VstPlugin::open(const char *path, uint32 flags){
    close();
	bool scGui = flags & Flags::ScGui;
    bool vstGui = flags & Flags::VstGui;
	// initialize GUI backend (if needed)
	if (vstGui) {
    #ifdef __APPLE__
        LOG_WARNING("Warning: VST GUI not supported (yet) on macOS!");
		scGui = true;
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
		scGui_ = scGui;
        vstGui_ = vstGui;
        paramDisplay_ = flags & Flags::ParamDisplay;
		LOG_DEBUG("loaded " << path);
		int blockSize = bufferSize();
		plugin_->setSampleRate(sampleRate());
		plugin_->setBlockSize(blockSize);
		if (plugin_->hasPrecision(VSTProcessPrecision::Single)) {
			plugin_->setPrecision(VSTProcessPrecision::Single);
		}
		else {
			LOG_WARNING("VstPlugin: plugin '" << plugin_->getPluginName() << "' doesn't support single precision processing - bypassing!");
		}
        resizeBuffer();
		// allocate arrays for parameter values/states
		int nParams = plugin_->getNumParameters();
		auto result = (Param *)RTRealloc(mWorld, paramStates_, nParams * sizeof(Param));
        if (result){
			paramStates_ = result;
            for (int i = 0; i < nParams; ++i) {
				paramStates_[i].value = std::numeric_limits<float>::quiet_NaN();
                paramStates_[i].bus = -1;
            }
        } else {
			RTFree(mWorld, paramStates_);
			paramStates_ = nullptr;
            LOG_ERROR("RTRealloc failed!");
        }
		sendPluginInfo();
		sendPrograms();
		sendMsg("/vst_open", 1);
		sendParameters(); // after open!
	} else {
		LOG_WARNING("VstPlugin: couldn't load " << path);
		sendMsg("/vst_open", 0);
	}
}

IVSTPlugin* VstPlugin::tryOpenPlugin(const char *path, bool gui){
#if VSTTHREADS
        // creates a new thread where the plugin is created and the message loop runs
    if (gui){
        std::promise<IVSTPlugin *> promise;
        auto future = promise.get_future();
		LOG_DEBUG("started thread");
        thread_ = std::thread(&VstPlugin::threadFunction, this, std::move(promise), path);
        return future.get();
    }
#endif
        // create plugin in main thread
    IVSTPlugin *plugin = loadVSTPlugin(makeVSTPluginFilePath(path));
        // receive events from plugin
    plugin->setListener(listener_.get());
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
void VstPlugin::threadFunction(std::promise<IVSTPlugin *> promise, const char *path){
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
    plugin->setListener(listener_.get());
        // return plugin to main thread
    promise.set_value(plugin);
        // setup GUI window (if any)
    if (window_){
        window_->setTitle(plugin->getPluginName());
        int left, top, right, bottom;
        plugin->getEditorRect(left, top, right, bottom);
        window_->setGeometry(left, top, right, bottom);

        plugin->openEditor(window_->getHandle());
            // run the event loop until the window is destroyed (which implicitly closes the editor)
        window_->run();
            // some plugins expect to released in the same thread where they have been created
        freeVSTPlugin(plugin);
		plugin_ = nullptr;
    }
}
#endif

void VstPlugin::showEditor(bool show) {
	if (plugin_ && window_) {
		if (show) {
			window_->bringToTop();
		}
		else {
			window_->hide();
		}
	}
}

void VstPlugin::reset() {
	if (check()) {
		plugin_->suspend();
		plugin_->resume();
	}
}

// perform routine
void VstPlugin::next(int inNumSamples) {
    if (!(buf_ && inBufVec_ && outBufVec_)) return;
    int nin = numInChannels_;
    int nout = numOutChannels_;
    bool bypass = in0(0);
	int offset = 0;

    // only reset plugin when bypass changed from true to false
    if (plugin_ && !bypass && (bypass != bypass_)) {
        reset();
    }
    bypass_ = bypass;

	// setup pointer arrays:
	for (int i = 0; i < nin; ++i) {
		inBufVec_[i] = in(i + inChannelOnset_);
	}
	for (int i = 0; i < nout; ++i) {
		outBufVec_[i] = out(i);
	}

	if (plugin_ && !bypass && plugin_->hasPrecision(VSTProcessPrecision::Single)) {
		if (paramStates_) {
			// update parameters from mapped control busses
			int maxControlChannel = mWorld->mNumControlBusChannels;
			int nparam = plugin_->getNumParameters();
			for (int i = 0; i < nparam; ++i) {
				int bus = paramStates_[i].bus;
				if (bus >= 0) {
					float value = readControlBus(bus, maxControlChannel);
					if (value != paramStates_[i].value) {
						plugin_->setParameter(i, value);
						paramStates_[i].value = value;
					}
				}
			}
			// update parameters from UGen inputs
			for (int i = 0; i < numParameterControls_; ++i) {
				int k = 2 * i + parameterControlOnset_;
				int index = in0(k);
				float value = in0(k + 1);
				// only if index is not out of range and the param is not mapped to a bus
				if (index >= 0 && index < nparam && paramStates_[index].bus < 0
					&& paramStates_[index].value != value)
				{
					plugin_->setParameter(index, value);
					paramStates_[index].value = value;
				}
			}
		}
        // process
		plugin_->process((const float **)inBufVec_, outBufVec_, inNumSamples);
		offset = plugin_->getNumOutputs();

#if VSTTHREADS
		// send parameter automation notification (only if we have a VST editor window)
		if (window_) {
			std::lock_guard<std::mutex> guard(mutex_);
			for (int i = 0; i < paramQueue_.size; ++i){
				auto& item = paramQueue_.data[i];
				parameterAutomated(item.index, item.value);
			}
			paramQueue_.size = 0; // dont' deallocate memory
		}
#endif
	}
    else {
        // bypass (copy input to output)
        int n = std::min(nin, nout);
		for (int i = 0; i < n; ++i) {
			Copy(inNumSamples, outBufVec_[i], (float *)inBufVec_[i]);
		}
		offset = n;
    }
	// zero remaining outlets
	for (int i = offset; i < nout; ++i) {
		Fill(inNumSamples, outBufVec_[i], 0.f);
	}
}

void VstPlugin::setParam(int32 index, float value) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumParameters()) {
			plugin_->setParameter(index, value);
			paramStates_[index].value = value;
			paramStates_[index].bus = -1; // invalidate bus num
			if (scGui_ && paramDisplay_) {
				const int maxSize = 64;
				float buf[maxSize];
				buf[0] = index;
				int len = string2floatArray(plugin_->getParameterDisplay(index), buf + 1, maxSize - 1);
				sendMsg("/vst_pd", len + 1, buf);
			}
		}
		else {
			LOG_WARNING("VstPlugin: parameter index " << index << " out of range!");
		}
	}
}

void VstPlugin::getParam(int32 index) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumParameters()) {
			float value = plugin_->getParameter(index);
			sendMsg("/vst_set", value);
		}
		else {
			LOG_WARNING("VstPlugin: parameter index " << index << " out of range!");
		}
	}
}

void VstPlugin::getParamN(int32 index, int32 count) {
	if (check()) {
		int32 nparam = plugin_->getNumParameters();
		if (index >= 0 && index < nparam) {
			count = std::min<int32>(count, nparam - index);
			const int bufsize = count + 1;
			float *buf = (float *)RTAlloc(mWorld, sizeof(float) * bufsize);
			if (buf) {
				buf[0] = count;
				for (int i = 0; i < count; ++i) {
					float value = plugin_->getParameter(i + index);
					buf[i + 1] = value;
				}
				sendMsg("/vst_setn", bufsize, buf);
				RTFree(mWorld, buf);
			}
			else {
				LOG_WARNING("RTAlloc failed!");
			}
		}
		else {
			LOG_WARNING("VstPlugin: parameter index " << index << " out of range!");
		}
	}
}

void VstPlugin::mapParam(int32 index, int32 bus) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumParameters()) {
			paramStates_[index].bus = bus;
		}
		else {
			LOG_WARNING("VstPlugin: parameter index " << index << " out of range!");
		}
	}
}

void VstPlugin::unmapParam(int32 index) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumParameters()) {
			paramStates_[index].bus = -1;
		}
		else {
			LOG_WARNING("VstPlugin: parameter index " << index << " out of range!");
		}
	}
}

// program/bank
void VstPlugin::setProgram(int32 index) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumPrograms()) {
			plugin_->setProgram(index);
            if (window_){
                window_->update();
            }
			sendMsg("/vst_pgm", index);
			sendParameters();
		}
		else {
			LOG_WARNING("VstPlugin: program number " << index << " out of range!");
		}
	}
}
void VstPlugin::setProgramName(const char *name) {
	if (check()) {
		plugin_->setProgramName(name);
		sendCurrentProgram();
	}
}
void VstPlugin::readProgram(const char *path) {
	if (check()) {
		if (plugin_->readProgramFile(path)) {
            if (window_){
                window_->update();
            }
			sendCurrentProgram();
			sendParameters();
		}
		else {
			LOG_WARNING("VstPlugin: couldn't read program file '" << path << "'");
		}
	}
}
void VstPlugin::writeProgram(const char *path) {
	if (check()) {
		plugin_->writeProgramFile(path);
	}
}
void VstPlugin::setProgramData(const char *data, int32 n) {
	if (check()) {
		if (plugin_->readProgramData(data, n)) {
            if (window_){
                window_->update();
            }
			sendCurrentProgram();
			sendParameters();
		}
		else {
			LOG_WARNING("VstPlugin: couldn't read program data");
		}
	}
}
void VstPlugin::getProgramData() {
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
					// no need to cast to unsigned because SC's Int8Array is signed
					buf[i] = data[i];
				}
				sendMsg("/vst_pgm_data", len, buf);
				RTFree(mWorld, buf);
			}
			else {
				LOG_ERROR("RTAlloc failed!");
			}
		}
		else {
			LOG_WARNING("VstPlugin: couldn't write program data");
		}
	}
}
void VstPlugin::readBank(const char *path) {
	if (check()) {
		if (plugin_->readBankFile(path)) {
            if (window_){
                window_->update();
            }
			sendPrograms();
			sendParameters();
			sendMsg("/vst_pgm", plugin_->getProgram());
		}
		else {
			LOG_WARNING("VstPlugin: couldn't read bank file '" << path << "'");
		}
	}
}
void VstPlugin::writeBank(const char *path) {
	if (check()) {
		plugin_->writeBankFile(path);
	}
}
void VstPlugin::setBankData(const char *data, int32 n) {
	if (check()) {
		if (plugin_->readBankData(data, n)) {
            if (window_){
                window_->update();
            }
			sendPrograms();
			sendParameters();
			sendMsg("/vst_pgm", plugin_->getProgram());
		}
		else {
			LOG_WARNING("VstPlugin: couldn't read bank data");
		}
	}
}
void VstPlugin::getBankData() {
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
					// no need to cast to unsigned because SC's Int8Array is signed
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
			LOG_WARNING("VstPlugin: couldn't write bank data");
		}
	}
}
// midi
void VstPlugin::sendMidiMsg(int32 status, int32 data1, int32 data2) {
	if (check()) {
		plugin_->sendMidiEvent(VSTMidiEvent(status, data1, data2));
	}
}
void VstPlugin::sendSysexMsg(const char *data, int32 n) {
	if (check()) {
		plugin_->sendSysexEvent(VSTSysexEvent(data, n));
	}
}
// transport
void VstPlugin::setTempo(float bpm) {
	if (check()) {
		plugin_->setTempoBPM(bpm);
	}
}
void VstPlugin::setTimeSig(int32 num, int32 denom) {
	if (check()) {
		plugin_->setTimeSignature(num, denom);
	}
}
void VstPlugin::setTransportPlaying(bool play) {
	if (check()) {
		plugin_->setTransportPlaying(play);
	}
}
void VstPlugin::setTransportPos(float pos) {
	if (check()) {
		plugin_->setTransportPosition(pos);
	}
}
void VstPlugin::getTransportPos() {
	if (check()) {
		float f = plugin_->getTransportPosition();
		sendMsg("/vst_transport", f);
	}
}

// helper methods

float VstPlugin::readControlBus(int32 num, int32 maxChannel) {
    if (num >= 0 && num < maxChannel) {
		return mWorld->mControlBus[num];
	}
	else {
		return 0.f;
	}
}

void VstPlugin::sendPluginInfo() {
    const int maxSize = 64;
    float buf[maxSize];

	// send plugin name:
    int nameLen = string2floatArray(plugin_->getPluginName(), buf, maxSize);
    sendMsg("/vst_name", nameLen, buf);

	// send plugin info (nin, nout, nparams, nprograms, flags, version):
	uint32 flags = 0;
	flags |= plugin_->hasEditor() << HasEditor;
	flags |= plugin_->isSynth() << IsSynth;
	flags |= plugin_->hasPrecision(VSTProcessPrecision::Single) << SinglePrecision;
	flags |= plugin_->hasPrecision(VSTProcessPrecision::Double) << DoublePrecision;
	flags |= plugin_->hasMidiInput() << MidiInput;
	flags |= plugin_->hasMidiOutput() << MidiOutput;
	int nparams = plugin_->getNumParameters();
	buf[0] = plugin_->getNumInputs();
	buf[1] = plugin_->getNumOutputs();
	buf[2] = nparams;
	buf[3] = plugin_->getNumPrograms();
	buf[4] = flags;
	buf[5] = plugin_->getPluginVersion();
	sendMsg("/vst_info", 6, buf);

	// send parameter names
	for (int i = 0; i < nparams; ++i) {
		// msg format: index, len, characters...
		buf[0] = i;
        int len = string2floatArray(plugin_->getParameterName(i), buf + 1, maxSize - 1);
		sendMsg("/vst_pn", len + 1, buf);
	}
	// send parameter labels
	if (paramDisplay_) {
		for (int i = 0; i < nparams; ++i) {
			// msg format: index, len, characters...
			buf[0] = i;
			int len = string2floatArray(plugin_->getParameterLabel(i), buf + 1, maxSize - 1);
			sendMsg("/vst_pl", len + 1, buf);
		}
	}
}

void VstPlugin::sendPrograms() {
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

bool VstPlugin::sendProgram(int32 num) {
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
	sendMsg("/vst_pgmn", len + 1, buf);
	return changed;
}

void VstPlugin::sendCurrentProgram() {
	const int maxSize = 64;
	float buf[maxSize];
	auto name = plugin_->getProgramName();
	// msg format: index, len, characters...
	buf[0] = plugin_->getProgram();
	int len = string2floatArray(name, buf + 1, maxSize - 1);
	sendMsg("/vst_pgmn", len + 1, buf);
}

void VstPlugin::sendParameters() {
	if (scGui_) {
		const int maxSize = 64;
		float buf[maxSize];
		int nparam = plugin_->getNumParameters();
		for (int i = 0; i < nparam; ++i) {
			buf[0] = i;
			buf[1] = plugin_->getParameter(i);
			sendMsg("/vst_pp", 2, buf);
			if (paramDisplay_) {
				int len = string2floatArray(plugin_->getParameterDisplay(i), buf + 1, maxSize - 1);
				sendMsg("/vst_pd", len + 1, buf);
			}
		}
	}
}

void VstPlugin::parameterAutomated(int32 index, float value) {
	float buf[2] = { (float)index, value };
	sendMsg("/vst_pa", 2, buf);
}

void VstPlugin::midiEvent(const VSTMidiEvent& midi) {
	float buf[3];
	// we don't want negative values here
	buf[0] = (unsigned char) midi.data[0];
	buf[1] = (unsigned char) midi.data[1];
	buf[2] = (unsigned char) midi.data[2];
	sendMsg("/vst_midi", 3, buf);
}

void VstPlugin::sysexEvent(const VSTSysexEvent& sysex) {
	auto& data = sysex.data;
	int size = data.size();
	float *buf = (float *)RTAlloc(mWorld, size * sizeof(float));
	if (buf) {
		for (int i = 0; i < size; ++i) {
			// no need to cast to unsigned because SC's Int8Array is signed anyway
			buf[i] = data[i];
		}
		sendMsg("/vst_sysex", size, buf);
		RTFree(mWorld, buf);
	}
	else {
		LOG_WARNING("RTAlloc failed!");
	}
}

void VstPlugin::sendMsg(const char *cmd, float f) {
	// LOG_DEBUG("sending msg: " << cmd);
	SendNodeReply(&mParent->mNode, mParentIndex, cmd, 1, &f);
}

void VstPlugin::sendMsg(const char *cmd, int n, const float *data) {
	// LOG_DEBUG("sending msg: " << cmd);
	SendNodeReply(&mParent->mNode, mParentIndex, cmd, n, data);
}

// unit command callbacks

#define CHECK {if (!static_cast<VstPlugin*>(unit)->valid()) return; }

void vst_open(Unit *unit, sc_msg_iter *args) {
	CHECK;
	uint32 flags = args->geti();
	const char *path = args->gets();
	if (path) {
        static_cast<VstPlugin*>(unit)->open(path, flags);
	}
	else {
		LOG_WARNING("vst_open: expecting string argument!");
	}
}

void vst_close(Unit *unit, sc_msg_iter *args) {
	CHECK;
	static_cast<VstPlugin*>(unit)->close();
}

void vst_reset(Unit *unit, sc_msg_iter *args) {
	CHECK;
	static_cast<VstPlugin*>(unit)->reset();
}

void vst_vis(Unit *unit, sc_msg_iter *args) {
	CHECK;
	bool show = args->geti();
	static_cast<VstPlugin*>(unit)->showEditor(show);
}

// set parameters given as pairs of index and value
void vst_set(Unit *unit, sc_msg_iter *args) {
	CHECK;
	auto vst = static_cast<VstPlugin*>(unit);
	if (vst->check()) {
		int nparam = vst->plugin()->getNumParameters();
		while (args->remain() > 0) {
			int32 index = args->geti();
			float value = args->getf();
			if (index >= 0 && index < nparam) {
				vst->setParam(index, value);
			}
		}
	}
}

// set parameters given as triples of index, count and values
void vst_setn(Unit *unit, sc_msg_iter *args) {
	CHECK;
	auto vst = static_cast<VstPlugin*>(unit);
	if (vst->check()) {
		int nparam = vst->plugin()->getNumParameters();
		while (args->remain() > 0) {
			int32 index = args->geti();
			int32 count = args->geti();
			for (int i = 0; i < count; ++i) {
				float value = args->getf();
				int32 idx = index + 1;
				if (idx >= 0 && idx < nparam) {
					vst->setParam(idx, value);
				}
			}
		}
	}
}

// get a single parameter at index
void vst_get(Unit *unit, sc_msg_iter *args) {
	CHECK;
	int32 index = args->geti(-1);
	static_cast<VstPlugin*>(unit)->getParam(index);
}

// get a number of parameters starting from index
void vst_getn(Unit *unit, sc_msg_iter *args) {
	CHECK;
	int32 index = args->geti();
	int32 count = args->geti();
	static_cast<VstPlugin*>(unit)->getParamN(index, count);
}

// map parameters to control busses
void vst_map(Unit *unit, sc_msg_iter *args) {
	CHECK;
	auto vst = static_cast<VstPlugin*>(unit);
	if (vst->check()) {
		int nparam = vst->plugin()->getNumParameters();
		while (args->remain() > 0) {
			int32 index = args->geti();
			int32 bus = args->geti(-1);
			int32 numChannels = args->geti();
			for (int i = 0; i < numChannels; ++i) {
				int32 idx = index + i;
				if (idx >= 0 && idx < nparam) {
					vst->mapParam(idx, bus + i);
				}
			}
		}
	}
}

// unmap parameters from control busses
void vst_unmap(Unit *unit, sc_msg_iter *args) {
	CHECK;
	auto vst = static_cast<VstPlugin*>(unit);
	if (vst->check()) {
		int nparam = vst->plugin()->getNumParameters();
		if (args->remain()) {
			do {
				int32 index = args->geti();
				if (index >= 0 && index < nparam) {
					vst->unmapParam(index);
				}
			} while (args->remain());
		}
		else {
			// unmap all parameters:
			for (int i = 0; i < nparam; ++i) {
				vst->unmapParam(i);
			}
		}
		
	}
}

void vst_program_set(Unit *unit, sc_msg_iter *args) {
	CHECK;
	int32 index = args->geti();
	static_cast<VstPlugin*>(unit)->setProgram(index);
}

void vst_program_get(Unit *unit, sc_msg_iter *args) {
	CHECK;
	int32 index = args->geti();
	static_cast<VstPlugin*>(unit)->setProgram(index);
}

void vst_program_name(Unit *unit, sc_msg_iter *args) {
	CHECK;
	const char *name = args->gets();
	if (name) {
		static_cast<VstPlugin*>(unit)->setProgramName(name);
	}
	else {
		LOG_WARNING("vst_program_name: expecting string argument!");
	}
}

void vst_program_read(Unit *unit, sc_msg_iter *args) {
	CHECK;
	const char *path = args->gets();
	if (path) {
		static_cast<VstPlugin*>(unit)->readProgram(path);
	}
	else {
		LOG_WARNING("vst_program_read: expecting string argument!");
	}
}

void vst_program_write(Unit *unit, sc_msg_iter *args) {
	CHECK;
	const char *path = args->gets();
	if (path) {
		static_cast<VstPlugin*>(unit)->writeProgram(path);
	}
	else {
		LOG_WARNING("vst_program_write: expecting string argument!");
	}
}

void vst_program_data_set(Unit *unit, sc_msg_iter *args) {
	CHECK;
	int len = args->getbsize();
	if (len > 0) {
		// LATER avoid unnecessary copying
		char *buf = (char *)RTAlloc(unit->mWorld, len);
		if (buf) {
			args->getb(buf, len);
			static_cast<VstPlugin*>(unit)->setProgramData(buf, len);
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
	CHECK;
	static_cast<VstPlugin*>(unit)->getProgramData();
}

void vst_bank_read(Unit *unit, sc_msg_iter *args) {
	CHECK;
	const char *path = args->gets();
	if (path) {
		static_cast<VstPlugin*>(unit)->readBank(path);
	}
	else {
		LOG_WARNING("vst_bank_read: expecting string argument!");
	}
}

void vst_bank_write(Unit *unit, sc_msg_iter *args) {
	CHECK;
	const char *path = args->gets();
	if (path) {
		static_cast<VstPlugin*>(unit)->writeBank(path);
	}
	else {
		LOG_WARNING("vst_bank_write: expecting string argument!");
	}
}

void vst_bank_data_set(Unit *unit, sc_msg_iter *args) {
	CHECK;
	int len = args->getbsize();
	if (len > 0) {
		// LATER avoid unnecessary copying
		char *buf = (char *)RTAlloc(unit->mWorld, len);
		if (buf) {
			args->getb(buf, len);
			static_cast<VstPlugin*>(unit)->setBankData(buf, len);
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
	CHECK;
	static_cast<VstPlugin*>(unit)->getBankData();
}


void vst_midi_msg(Unit *unit, sc_msg_iter *args) {
	CHECK;
	char data[4];
	int32 len = args->getbsize();
	if (len > 4) {
		LOG_WARNING("vst_midi_msg: midi message too long (" << len << " bytes)");
	}
	args->getb(data, len);
	static_cast<VstPlugin*>(unit)->sendMidiMsg(data[0], data[1], data[2]);
}

void vst_midi_sysex(Unit *unit, sc_msg_iter *args) {
	CHECK;
	int len = args->getbsize();
	if (len > 0) {
		// LATER avoid unnecessary copying
		char *buf = (char *)RTAlloc(unit->mWorld, len);
		if (buf) {
			args->getb(buf, len);
			static_cast<VstPlugin*>(unit)->sendSysexMsg(buf, len);
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
	CHECK;
	float bpm = args->getf();
	static_cast<VstPlugin*>(unit)->setTempo(bpm);
}

void vst_time_sig(Unit *unit, sc_msg_iter *args) {
	CHECK;
	int32 num = args->geti();
	int32 denom = args->geti();
	static_cast<VstPlugin*>(unit)->setTimeSig(num, denom);
}

void vst_transport_play(Unit *unit, sc_msg_iter *args) {
	CHECK;
	int play = args->geti();
	static_cast<VstPlugin*>(unit)->setTransportPlaying(play);
}

void vst_transport_set(Unit *unit, sc_msg_iter *args) {
	CHECK;
	float pos = args->getf();
	static_cast<VstPlugin*>(unit)->setTransportPos(pos);
}

void vst_transport_get(Unit *unit, sc_msg_iter *args) {
	CHECK;
	static_cast<VstPlugin*>(unit)->getTransportPos();
}

void vst_poll(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
    VSTWindowFactory::mainLoopPoll();
}

void VstPlugin_Ctor(VstPlugin* unit){
	new(unit)VstPlugin();
}

void VstPlugin_Dtor(VstPlugin* unit){
	unit->~VstPlugin();
}

#define DefineCmd(x) DefineUnitCmd("VstPlugin", "/" #x, vst_##x)

PluginLoad(VstPlugin) {
    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable
	DefineDtorCantAliasUnit(VstPlugin);
	DefineCmd(open);
	DefineCmd(close);
	DefineCmd(reset);
	DefineCmd(vis);
	DefineCmd(set);
	DefineCmd(setn);
	DefineCmd(get);
	DefineCmd(getn);
	DefineCmd(map);
	DefineCmd(unmap);
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
