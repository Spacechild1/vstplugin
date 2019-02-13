#include "VstPluginUGen.h"
#include "Utility.h"

#include <limits>
#include <fstream>
#include <set>
#ifdef _WIN32
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else // just because of Clang on macOS not shipping <experimental/filesystem>...
#include <dirent.h>
#endif
static InterfaceTable *ft;

void SCLog(const std::string& msg){
	Print(msg.c_str());
}

static const char * platformExtensions[] = {
#ifdef APPLE
	".vst",
#endif
#ifdef _WIN32
	".dll",
#endif
#ifdef __linux__
	".so",
#endif
	nullptr
};

static const char * defaultSearchPaths[] = {
// macOS
#ifdef APPLE
	"~/Library/Audio/Plug-Ins/VST", "/Library/Audio/Plug-Ins/VST",
#endif
// Windows
#ifdef _WIN32
	// LATER get %PROGRAMFILES% from environment
#ifdef _WIN64 // 64 bit
#define PROGRAMFILES "C:\\Program Files\\"
#else // 32 bit
#define PROGRAMFILES "C:\\Program Files (86)\\"
#endif
	PROGRAMFILES "VSTPlugins", PROGRAMFILES "Steinberg\\VSTPlugins\\",
	PROGRAMFILES "Common Files\\VST2\\", PROGRAMFILES "Common Files\\Steinberg\\VST2\\",
#undef PROGRAMFILES
#endif
// Linux
#ifdef __linux__
	"/usr/lib/vst", "/usr/local/lib/vst",
#endif
	nullptr
};
static std::vector<std::string> userSearchPaths;
static std::vector<std::string> pluginList;
static VstPluginMap pluginMap;
static bool isSearching = false;

// sending reply OSC messages
// we only need int, float and string

int doAddArg(char *buf, int size, int i) {
	return snprintf(buf, size, "%d\n", i);
}

int doAddArg(char *buf, int size, float f) {
	return snprintf(buf, size, "%f\n", f);
}

int doAddArg(char *buf, int size, const char *s) {
	return snprintf(buf, size, "%s\n", s);
}

int doAddArg(char *buf, int size, const std::string& s) {
	return snprintf(buf, size, "%s\n", s.c_str());
}

// end condition
int addArgs(char *buf, int size) { return 0; }

// recursively add arguments
template<typename Arg, typename... Args>
int addArgs(char *buf, int size, Arg&& arg, Args&&... args) {
	auto n = doAddArg(buf, size, std::forward<Arg>(arg));
	if (n >= 0 && n < size) {
		n += addArgs(buf + n, size - n, std::forward<Args>(args)...);
	}
	return n;
}

template<typename... Args>
int makeReply(char *buf, int size, const char *address, Args&&... args) {
	auto n = snprintf(buf, size, "%s\n", address);
	if (n >= 0 && n < size) {
		n += addArgs(buf + n, size - n, std::forward<Args>(args)...);
	}
	if (n > 0) {
		buf[n - 1] = 0; // remove trailing \n
	}
	return n;
}

#if 0
template<typename... Args>
void sendReply(World *world, void *replyAddr, const char *s, int size) {
	// "abuse" DoAsynchronousCommand to send a reply string
	// unfortunately, we can't send OSC messages directly, so I have to assemble a string
	// (with the arguments seperated by newlines) which is send as the argument for a /done message.
	// 'cmdName' isn't really copied, so we have to make sure it outlives stage 4.
	auto data = (char *)RTAlloc(world, size + 1);
	if (data) {
		LOG_DEBUG("send reply");
		memcpy(data, s, size);
		data[size] = 0;
		auto deleter = [](World *inWorld, void *cmdData) {
			LOG_DEBUG("delete reply");
			RTFree(inWorld, cmdData);
		};
		DoAsynchronousCommand(world, replyAddr, data, data, 0, 0, 0, deleter, 0, 0);
	}
	else {
		LOG_ERROR("RTAlloc failed!");
	}
}
#endif

	// format: size, chars...
int string2floatArray(const std::string& src, float *dest, int maxSize) {
	int len = std::min<int>(src.size(), maxSize-1);
	if (len >= 0) {
		*dest++ = len;
		for (int i = 0; i < len; ++i) {
			dest[i] = src[i];
		}
		return len + 1;
	}
	else {
		return 0;
	}
}

bool cmdNRTfree(World *world, void *cmdData) {
	((VstPluginCmdData *)cmdData)->~VstPluginCmdData();
	// LOG_DEBUG("cmdNRTfree");
	return true;
}

void cmdRTfree(World *world, void* cmdData) {
	RTFree(world, cmdData);
	// LOG_DEBUG("cmdRTfree!");
}

// VstPluginListener

VstPluginListener::VstPluginListener(VstPlugin &owner)
	: owner_(&owner) {}

void VstPluginListener::parameterAutomated(int index, float value) {
#if VSTTHREADS
	// only push it to the queue when we're not in the realtime thread
	if (std::this_thread::get_id() != owner_->rtThreadID_) {
		std::lock_guard<std::mutex> guard(owner_->mutex_);
		owner_->paramQueue_.emplace_back(index, value);
	}
	else {
#else
	{
#endif
		owner_->parameterAutomated(index, value);
	}
}

void VstPluginListener::midiEvent(const VSTMidiEvent& midi) {
#if VSTTHREADS
	// check if we're on the realtime thread, otherwise ignore it
	if (std::this_thread::get_id() == owner_->rtThreadID_) {
#else
	{
#endif
		owner_->midiEvent(midi);
	}
}

void VstPluginListener::sysexEvent(const VSTSysexEvent& sysex) {
#if VSTTHREADS
	// check if we're on the realtime thread, otherwise ignore it
	if (std::this_thread::get_id() == owner_->rtThreadID_) {
#else
	{
#endif
		owner_->sysexEvent(sysex);
	}
}

// VstPlugin

VstPlugin::VstPlugin(){
#if VSTTHREADS
	rtThreadID_ = std::this_thread::get_id();
	// LOG_DEBUG("thread ID constructor: " << rtThreadID_);
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
	LOG_DEBUG("destroyed VstPlugin");
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

bool cmdClose(World *world, void* cmdData) {
	((VstPluginCmdData *)cmdData)->close();
	return true;
}

	// try to close the plugin in the NRT thread with an asynchronous command
void VstPlugin::close() {
	if (plugin_) {
		auto cmdData = makeCmdData();
		if (!cmdData) {
			return;
		}
		// plugin, window and thread don't depend on VstPlugin so they can be
		// safely moved to cmdData (which takes care of the actual closing)
		cmdData->plugin = plugin_;
		cmdData->window = window_;
#if VSTTHREADS
		cmdData->thread = std::move(thread_);
#endif
		doCmd(cmdData, cmdClose);
		window_ = nullptr;
		plugin_ = nullptr;
	}
}

void VstPluginCmdData::close() {
	if (!plugin) return;

	if (VSTTHREADS && window) {
		// terminate the message loop (will implicitly release the plugin)
		window->quit();
		// now join the thread
		if (thread.joinable()) {
			thread.join();
			LOG_DEBUG("thread joined");
		}
		// then destroy the window
		window = nullptr;
	}
	else {
		// first destroy the window (if any)
		window = nullptr;
		// then release the plugin
		freeVSTPlugin(plugin);
	}
	LOG_DEBUG("VST plugin closed");
}

bool cmdOpen(World *world, void* cmdData) {
	LOG_DEBUG("cmdOpen");
	// initialize GUI backend (if needed)
	auto data = (VstPluginCmdData *)cmdData;
	if (data->value) { // VST gui?
#ifdef __APPLE__
		LOG_WARNING("Warning: VST GUI not supported (yet) on macOS!");
		data->value = false;
#else
		static bool initialized = false;
		if (!initialized) {
			VSTWindowFactory::initialize();
			initialized = true;
		}
#endif
	}
	data->tryOpen();
	return true;
}

bool cmdOpenDone(World *world, void* cmdData) {
	auto data = (VstPluginCmdData *)cmdData;
	data->owner->doneOpen(*data);
	return true;
}

	// try to open the plugin in the NRT thread with an asynchronous command
void VstPlugin::open(const char *path, bool gui) {
	LOG_DEBUG("open");
	if (isLoading_) {
		LOG_WARNING("already loading!");
		return;
	}
	close();
	if (plugin_) {
		LOG_ERROR("couldn't close current plugin!");
		return;
	}
	
    auto cmdData = makeCmdData(path);
	if (cmdData) {
		cmdData->value = gui;
		doCmd(cmdData, cmdOpen, cmdOpenDone);
		isLoading_ = true;
    }
}

void VstPlugin::doneOpen(VstPluginCmdData& cmd){
	LOG_DEBUG("doneOpen");
	isLoading_ = false;
	plugin_ = cmd.plugin;
	window_ = cmd.window;
#if VSTTHREADS
	thread_ = std::move(cmd.thread);
#endif
    if (plugin_){
		LOG_DEBUG("loaded " << cmd.buf);
		plugin_->setListener(listener_.get());
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
		// success, window
		float data[] = { 1, (window_ != nullptr) };
		sendMsg("/vst_open", 2, data);
	} else {
		LOG_WARNING("VstPlugin: couldn't load " << cmd.buf);
		sendMsg("/vst_open", 0);
	}
}

#if VSTTHREADS
using VstPluginCmdPromise = std::promise<std::pair<IVSTPlugin *, std::shared_ptr<IVSTWindow>>>;
void threadFunction(VstPluginCmdPromise promise, const char *path);
#endif

void VstPluginCmdData::tryOpen(){
#if VSTTHREADS
        // creates a new thread where the plugin is created and the message loop runs
    if (value){ // VST gui?
		VstPluginCmdPromise promise;
        auto future = promise.get_future();
		LOG_DEBUG("started thread");
        thread = std::thread(threadFunction, std::move(promise), buf);
			// wait for thread to return the plugin and window
        auto result = future.get();
		LOG_DEBUG("got result from thread");
		plugin = result.first;
		window = result.second;
		if (!window) {
			thread.join(); // to avoid a crash in ~VstPluginCmdData
		}
		return;
    }
#endif
        // create plugin in main thread
    plugin = loadVSTPlugin(makeVSTPluginFilePath(buf));
#if !VSTTHREADS
        // create and setup GUI window in main thread (if needed)
    if (plugin && plugin->hasEditor() && value){
        window = std::shared_ptr<IVSTWindow>(VSTWindowFactory::create(plugin));
        if (window){
			window->setTitle(plugin->getPluginName());
            int left, top, right, bottom;
            plugin->getEditorRect(left, top, right, bottom);
			window->setGeometry(left, top, right, bottom);
            // don't open the editor on macOS (see VSTWindowCocoa.mm)
#ifndef __APPLE__
            plugin->openEditor(window->getHandle());
#endif
        }
    }
#endif
}

#if VSTTHREADS
void threadFunction(VstPluginCmdPromise promise, const char *path){
    IVSTPlugin *plugin = loadVSTPlugin(makeVSTPluginFilePath(path));
    if (!plugin){
            // signal other thread
		promise.set_value({ nullptr, nullptr });
        return;
    }
        // create GUI window (if needed)
	std::shared_ptr<IVSTWindow> window;
    if (plugin->hasEditor()){
        window = std::shared_ptr<IVSTWindow>(VSTWindowFactory::create(plugin));
    }
        // return plugin and window to other thread
	promise.set_value({ plugin, window });
        // setup GUI window (if any)
    if (window){
        window->setTitle(plugin->getPluginName());
        int left, top, right, bottom;
        plugin->getEditorRect(left, top, right, bottom);
        window->setGeometry(left, top, right, bottom);

        plugin->openEditor(window->getHandle());
            // run the event loop until it gets a quit message 
			// (the editor will we closed implicitly)
		LOG_DEBUG("start message loop");
		window->run();
		LOG_DEBUG("end message loop");
            // some plugins expect to released in the same thread where they have been created.
        freeVSTPlugin(plugin);
    }
}
#endif

bool cmdShowEditor(World *world, void *cmdData) {
	auto data = (VstPluginCmdData *)cmdData;
	if (data->value) {
		data->window->bringToTop();
	}
	else {
		data->window->hide();
	}
	return true;
}

void VstPlugin::showEditor(bool show) {
	if (plugin_ && window_) {
        auto cmdData = makeCmdData();
		if (cmdData) {
			cmdData->window = window_;
			cmdData->value = show;
			doCmd(cmdData, cmdShowEditor);
		}
	}
}

// some plugins crash when being reset reset
// in the NRT thread. we let the user choose
// and add a big fat warning in the help file.
bool cmdReset(World *world, void *cmdData) {
	auto data = (VstPluginCmdData *)cmdData;
	data->owner->plugin()->suspend();
	data->owner->plugin()->resume();
	return true;
}

void VstPlugin::reset(bool nrt) {
	if (check()) {
		if (nrt) {
			// reset in the NRT thread (unsafe)
			doCmd(makeCmdData(), cmdReset);
		}
		else {
			// reset in the RT thread (safe)
			plugin_->suspend();
			plugin_->resume();
		}
	}
}

// perform routine
void VstPlugin::next(int inNumSamples) {
    if (!(buf_ && inBufVec_ && outBufVec_)) return;
    int nin = numInChannels_;
    int nout = numOutChannels_;
    bool bypass = in0(0);
	int offset = 0;
	// disable reset for realtime-safety reasons
#if 0
    // only reset plugin when bypass changed from true to false
    if (plugin_ && !bypass && (bypass != bypass_)) {
        reset();
    }
    bypass_ = bypass;
#endif
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
		// send parameter automation notification posted from another thread.
		// we assume this is only possible if we have a VST editor window.
		// try_lock() won't block the audio thread and we don't mind if
		// notifications will be delayed if try_lock() fails (which happens rarely in practice).
		if (window_ && mutex_.try_lock()) {
			std::vector<std::pair<int, float>> queue;
			queue.swap(paramQueue_);
			mutex_.unlock();
			for (auto& p : queue) {
				parameterAutomated(p.first, p.second);
			}
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
			sendParameter(index);
		}
		else {
			LOG_WARNING("VstPlugin: parameter index " << index << " out of range!");
		}
	}
}

void VstPlugin::setParam(int32 index, const char *display) {
	if (check()) {
		if (index >= 0 && index < plugin_->getNumParameters()) {
			plugin_->setParameter(index, display);
			paramStates_[index].value = plugin_->getParameter(index);
			paramStates_[index].bus = -1; // invalidate bus num
			sendParameter(index);
		}
		else {
			LOG_WARNING("VstPlugin: parameter index " << index << " out of range!");
		}
	}
}

void VstPlugin::queryParams(int32 index, int32 count) {
	if (check()) {
		int32 nparam = plugin_->getNumParameters();
		if (index >= 0 && index < nparam) {
			count = std::min<int32>(count, nparam - index);
			for (int i = 0; i < count; ++i) {
				sendParameter(index + i);
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

void VstPlugin::getParams(int32 index, int32 count) {
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
			sendMsg("/vst_program_index", index);
		}
		else {
			LOG_WARNING("VstPlugin: program number " << index << " out of range!");
		}
	}
}
void VstPlugin::setProgramName(const char *name) {
	if (check()) {
		plugin_->setProgramName(name);
		sendCurrentProgramName();
	}
}

void VstPlugin::queryPrograms(int32 index, int32 count) {
	if (check()) {
		int32 nprogram = plugin_->getNumPrograms();
		if (index >= 0 && index < nprogram) {
			count = std::min<int32>(count, nprogram - index);
			auto old = plugin_->getProgram();
			bool changed;
			for (int i = 0; i < count; ++i) {
				changed = sendProgramName(index + i);
			}
			if (changed) {
				plugin_->setProgram(old);
			}
		}
		else {
			LOG_WARNING("VstPlugin: parameter index " << index << " out of range!");
		}
	}
}

bool cmdReadFile(World *world, void *cmdData, bool bank){
    auto data = (VstPluginCmdData *)cmdData;
    // open file
    std::ifstream file(data->buf, std::ios_base::binary);
    if (file.is_open()){
        // read from file
        file.seekg(0, std::ios_base::end);
		auto& buffer = data->mem;
        buffer.resize(file.tellg());
        file.seekg(0, std::ios_base::beg);
        file.read(&buffer[0], buffer.size());
        if (bank){
            if (data->owner->plugin()->readBankData(buffer)){
                LOG_DEBUG("file " << data->buf << " read!");
            } else {
                LOG_WARNING("couldn't read bank file \"" << data->buf << "\"!");
				buffer.clear(); // avoid setting data in cmdReadBankDone
            }
        } else {
            if (data->owner->plugin()->readProgramData(buffer)){
                LOG_DEBUG("file " << data->buf << " read!");
            } else {
                LOG_WARNING("couldn't read program file \"" << data->buf << "\"!");
				buffer.clear(); // avoid setting data in cmdReadProgramDone
            }
        }
    } else {
        LOG_WARNING("couldn't open file \"" << data->buf << "\"!");
    }
	return true;
}

bool cmdReadProgram(World *world, void *cmdData){
    return cmdReadFile(world, cmdData, false);
}

bool cmdReadBank(World *world, void *cmdData){
    return cmdReadFile(world, cmdData, true);
}

bool cmdReadProgramDone(World *world, void *cmdData){
    auto data = (VstPluginCmdData *)cmdData;
	auto& buffer = data->mem;
	if (buffer.size()) {
		data->owner->setProgramData(buffer.data(), buffer.size());
	}
    return true;
}

bool cmdReadBankDone(World *world, void *cmdData){
	auto data = (VstPluginCmdData *)cmdData;
	auto& buffer = data->mem;
	if (buffer.size()) {
		data->owner->setBankData(buffer.data(), buffer.size());
	}
	return true;
}

void VstPlugin::readProgram(const char *path){
    if (check()){
		doCmd(makeCmdData(path), cmdReadProgram, cmdReadProgramDone);
    }
}

void VstPlugin::readBank(const char *path) {
	if (check()) {
		doCmd(makeCmdData(path), cmdReadBank, cmdReadBankDone);
	}
}

void VstPlugin::sendData(int32 totalSize, int32 onset, const char *data, int32 n, bool bank) {
	LOG_DEBUG("got packet: " << totalSize << " (total size), " << onset << " (onset), " << n << " (size)");
	// first packet only
	if (onset == 0) {
		if (dataReceived_ != 0) {
			LOG_WARNING("last data hasn't been sent completely!");
		}
		dataReceived_ = 0;
		auto result = RTRealloc(mWorld, dataRT_, totalSize);
		if (result) {
			dataRT_ = (char *)result;
			dataSize_ = totalSize;
		}
		else {
			dataSize_ = 0;
			return;
		}
	}
	else if (onset < 0 || onset >= dataSize_) {
		LOG_ERROR("bug: bad onset!");
		return;
	}
	// append data
	auto size = dataSize_;
	if (size > 0) {
		if (n > (size - onset)) {
			LOG_ERROR("bug: data exceeding total size!");
			n = size - onset;
		}
		memcpy(dataRT_ + onset, data, n);
		if (onset != dataReceived_) {
			LOG_WARNING("onset and received data out of sync!");
		}
		dataReceived_ += n;
		LOG_DEBUG("data received: " << dataReceived_);
		// finished?
		if (dataReceived_ >= size) {
			if (bank) {
				setBankData(dataRT_, size);
			}
			else {
				setProgramData(dataRT_, size);
			}
			dataReceived_ = 0;
		}
	}
}

void VstPlugin::setProgramData(const char *data, int32 n) {
	if (check()) {
		if (!(data && n > 0)) {
			LOG_WARNING("VstPlugin: program data empty!");
			return;
		}
		if (plugin_->readProgramData(data, n)) {
			if (window_) {
				window_->update();
			}
			sendMsg("/vst_program_read", 1);
			sendCurrentProgramName();
		}
		else {
			LOG_WARNING("VstPlugin: couldn't read program data");
		}
	}
	sendMsg("/vst_program_read", 0);
}

void VstPlugin::setBankData(const char *data, int32 n) {
	if (check()) {
		if (!(data && n > 0)) {
			LOG_WARNING("VstPlugin: bank data empty!");
			return;
		}
		if (plugin_->readBankData(data, n)) {
			if (window_) {
				window_->update();
			}
			sendMsg("/vst_bank_read", 1);
			sendMsg("/vst_program_index", plugin_->getProgram());
		}
		else {
			LOG_WARNING("VstPlugin: couldn't read bank data");
		}
	}
	sendMsg("/vst_bank_read", 0);
}

bool cmdWriteFile(World *world, void *cmdData, bool bank){
    auto data = (VstPluginCmdData *)cmdData;
    // create file
    std::ofstream file(data->buf, std::ios_base::binary);
    if (file.is_open()){
		std::string buffer;
        // write to file
        if (bank){
            data->owner->plugin()->writeBankData(buffer);
        } else {
            data->owner->plugin()->writeProgramData(buffer);
        }
        file << buffer;
        LOG_DEBUG("file " << data->buf << " written!");
		data->value = true;
    } else {
        LOG_WARNING("couldn't write file \"" << data->buf << "\"!");
		data->value = false;
    }
	return true;
}

bool cmdWriteProgram(World *world, void *cmdData){
    return cmdWriteFile(world, cmdData, false);
}

bool cmdWriteBank(World *world, void *cmdData){
	std::string buffer; // will be thrown away
    return cmdWriteFile(world, cmdData, true);
}

bool cmdWriteProgramDone(World *world, void *cmdData){
    auto data = (VstPluginCmdData *)cmdData;
	data->owner->sendMsg("/vst_program_write", data->value);
    return true;
}

bool cmdWriteBankDone(World *world, void *cmdData) {
	auto data = (VstPluginCmdData *)cmdData;
	data->owner->sendMsg("/vst_bank_write", data->value);
	return true;
}

void VstPlugin::writeProgram(const char *path) {
    if (check()) {
		doCmd(makeCmdData(path), cmdWriteProgram, cmdWriteProgramDone);
	}
}

void VstPlugin::writeBank(const char *path) {
	if (check()) {
		doCmd(makeCmdData(path), cmdWriteBank, cmdWriteBankDone);
	}
}

bool VstPlugin::cmdGetData(World *world, void *cmdData, bool bank) {
	auto data = (VstPluginCmdData *)cmdData;
	auto owner = data->owner;
	auto& buffer = owner->dataNRT_;
	int count = data->value;
	if (count == 0) {
		// write whole program/bank data into buffer
		if (bank) {
			owner->plugin()->writeBankData(buffer);
		}
		else {
			owner->plugin()->writeProgramData(buffer);
		}
		owner->dataSent_ = 0;
		LOG_DEBUG("total data size: " << buffer.size());
	}
	// data left to send?
	auto onset = owner->dataSent_;
	auto remaining = buffer.size() - onset;
	if (remaining > 0) {
		// we want to send floats (but size_ is the number of bytes)
		int maxArgs = data->size / sizeof(float);
		// leave space for 3 extra arguments
		auto size = std::min<int>(remaining, maxArgs - 3);
		auto buf = (float *)data->buf;
		buf[0] = buffer.size(); // total
		buf[1] = onset; // onset
		buf[2] = size; // packet size
		// copy packet to cmdData's RT buffer
		auto packet = buffer.data() + onset;
		for (int i = 0; i < size; ++i) {
			// no need to cast to unsigned because SC's Int8Array is signed anyway
			buf[i + 3] = packet[i];
		}
		data->size = size + 3; // size_ becomes the number of float args
		owner->dataSent_ += size;
		LOG_DEBUG("send packet: " << buf[0] << " (total), " << buf[1] << " (onset), " << buf[2] << " (size)");
	}
	else {
		// avoid sending packet
		data->size = 0;
		// free program/bank data
		std::string empty;
		buffer.swap(empty);
		data->owner->dataSent_ = 0;
		LOG_DEBUG("done! free data");
	}
	return true;
}

bool VstPlugin::cmdGetDataDone(World *world, void *cmdData, bool bank) {
	auto data = (VstPluginCmdData *)cmdData;
	if (data->size > 0) {
		if (bank) {
			data->owner->sendMsg("/vst_bank_data", data->size, (const float *)data->buf);
		}
		else {
			data->owner->sendMsg("/vst_program_data", data->size, (const float *)data->buf);
		}
	}
	return true;
}

void VstPlugin::receiveProgramData(int count) {
	if (check()) {
		auto data = makeCmdData(MAX_OSC_PACKET_SIZE);
		if (data) {
			data->value = count;
			doCmd(data, [](World *world, void *cmdData) { return cmdGetData(world, cmdData, false); }, 
				[](World *world, void *cmdData) { return cmdGetDataDone(world, cmdData, false); });
		}
	}
}

void VstPlugin::receiveBankData(int count) {
	if (check()) {
		auto data = makeCmdData(MAX_OSC_PACKET_SIZE);
		if (data) {
			data->value = count;
			doCmd(data, [](World *world, void *cmdData) { return cmdGetData(world, cmdData, true); },
				[](World *world, void *cmdData) { return cmdGetDataDone(world, cmdData, true); });
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

// advanced

void VstPlugin::canDo(const char *what) {
	if (check()) {
		auto result = plugin_->canDo(what);
		sendMsg("/vst_can_do", (float)result);
	}
}

void VstPlugin::vendorSpecific(int32 index, int32 value, void *ptr, float opt) {
	if (check()) {
		auto result = plugin_->vedorSpecific(index, value, ptr, opt);
		sendMsg("/vst_vendor_method", (float)result);
	}
}

/*** helper methods ***/

float VstPlugin::readControlBus(int32 num, int32 maxChannel) {
    if (num >= 0 && num < maxChannel) {
		return mWorld->mControlBus[num];
	}
	else {
		return 0.f;
	}
}

// unchecked
bool VstPlugin::sendProgramName(int32 num) {
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
    int size = string2floatArray(name, buf + 1, maxSize - 1);
	sendMsg("/vst_program", size + 1, buf);
	return changed;
}

void VstPlugin::sendCurrentProgramName() {
	const int maxSize = 64;
	float buf[maxSize];
	// msg format: index, len, characters...
	buf[0] = plugin_->getProgram();
	int size = string2floatArray(plugin_->getProgramName(), buf + 1, maxSize - 1);
	sendMsg("/vst_program", size + 1, buf);
}

// unchecked
void VstPlugin::sendParameter(int32 index) {
	const int maxSize = 64;
	float buf[maxSize];
	// msg format: index, value, display length, display chars...
	buf[0] = index;
	buf[1] = plugin_->getParameter(index);
	int size = string2floatArray(plugin_->getParameterDisplay(index), buf + 2, maxSize - 2);
	sendMsg("/vst_param", size + 2, buf);
}

void VstPlugin::parameterAutomated(int32 index, float value) {
	sendParameter(index);
	float buf[2] = { (float)index, value };
	sendMsg("/vst_auto", 2, buf);
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
	if ((size * sizeof(float)) > MAX_OSC_PACKET_SIZE) {
		LOG_WARNING("sysex message (" << size << " bytes) too large for UDP packet - dropped!");
		return;
	}
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

VstPluginCmdData* VstPlugin::makeCmdData(const char *data, size_t size){
    VstPluginCmdData *cmdData = (VstPluginCmdData *)RTAlloc(mWorld, sizeof(VstPluginCmdData) + size);
    if (cmdData){
        new (cmdData) VstPluginCmdData();
        cmdData->owner = this;
        if (data){
            memcpy(cmdData->buf, data, size);
        }
		cmdData->size = size; // independent from data
	}
	else {
		LOG_ERROR("RTAlloc failed!");
	}
	return cmdData;
}

VstPluginCmdData* VstPlugin::makeCmdData(const char *path){
    size_t len = path ? (strlen(path) + 1) : 0;
    return makeCmdData(path, len);
}

VstPluginCmdData* VstPlugin::makeCmdData(size_t size) {
	return makeCmdData(nullptr, size);
}

VstPluginCmdData* VstPlugin::makeCmdData() {
	return makeCmdData(nullptr, 0);
}

void VstPlugin::doCmd(VstPluginCmdData *cmdData, AsyncStageFn nrt,
	AsyncStageFn rt) {
		// so we don't have to always check the return value of makeCmdData
	if (cmdData) {
		DoAsynchronousCommand(mWorld, 0, 0, cmdData, nrt, rt, cmdNRTfree, cmdRTfree, 0, 0);
	}
}

/*** unit command callbacks ***/

#define CHECK_UNIT {if (!CAST_UNIT->valid()) return; }
#define CAST_UNIT (static_cast<VstPlugin*>(unit))

void vst_open(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char *path = args->gets();
	auto gui = args->geti();
	if (path) {
        CAST_UNIT->open(path, gui);
	}
	else {
		LOG_WARNING("vst_open: expecting string argument!");
	}
}

void vst_close(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	CAST_UNIT->close();
}

void vst_reset(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	bool nrt = args->geti();
	CAST_UNIT->reset(nrt);
}

void vst_vis(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	bool show = args->geti();
	CAST_UNIT->showEditor(show);
}

// set parameters given as pairs of index and value
void vst_set(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	auto vst = CAST_UNIT;
	if (vst->check()) {
		while (args->remain() > 0) {
			int32 index = args->geti();
			if (args->remain() > 0 && args->nextTag() == 's') {
				vst->setParam(index, args->gets());
			}
			else {
				vst->setParam(index, args->getf());
			}
		}
	}
}

// set parameters given as triples of index, count and values
void vst_setn(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	auto vst = CAST_UNIT;
	if (vst->check()) {
		int nparam = vst->plugin()->getNumParameters();
		while (args->remain() > 0) {
			int32 index = args->geti();
			int32 count = args->geti();
			for (int i = 0; i < count && args->remain() > 0; ++i) {
				if (args->nextTag() == 's') {
					vst->setParam(index + i, args->gets());
				}
				else {
					vst->setParam(index + i, args->getf());
				}
			}
		}
	}
}

// query parameters starting from index (values + displays)
void vst_param_query(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 index = args->geti();
	int32 count = args->geti();
	CAST_UNIT->queryParams(index, count);
}

// get a single parameter at index (only value)
void vst_get(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 index = args->geti(-1);
	CAST_UNIT->getParam(index);
}

// get a number of parameters starting from index (only values)
void vst_getn(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 index = args->geti();
	int32 count = args->geti();
	CAST_UNIT->getParams(index, count);
}

// map parameters to control busses
void vst_map(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	auto vst = CAST_UNIT;
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
	CHECK_UNIT;
	auto vst = CAST_UNIT;
	if (vst->check()) {
		int nparam = vst->plugin()->getNumParameters();
		if (args->remain() > 0) {
			do {
				int32 index = args->geti();
				if (index >= 0 && index < nparam) {
					vst->unmapParam(index);
				}
			} while (args->remain() > 0);
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
	CHECK_UNIT;
	int32 index = args->geti();
	CAST_UNIT->setProgram(index);
}

// query parameters (values + displays) starting from index
void vst_program_query(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 index = args->geti();
	int32 count = args->geti();
	CAST_UNIT->queryPrograms(index, count);
}

void vst_program_name(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char *name = args->gets();
	if (name) {
		CAST_UNIT->setProgramName(name);
	}
	else {
		LOG_WARNING("vst_program_name: expecting string argument!");
	}
}

void vst_program_read(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char *path = args->gets();
	if (path) {
		CAST_UNIT->readProgram(path);
	}
	else {
		LOG_WARNING("vst_program_read: expecting string argument!");
	}
}

void vst_program_write(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char *path = args->gets();
	if (path) {
		CAST_UNIT->writeProgram(path);
	}
	else {
		LOG_WARNING("vst_program_write: expecting string argument!");
	}
}

void vst_program_data_set(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int totalSize = args->geti();
	int onset = args->geti();
	int len = args->getbsize();
	if (len > 0) {
		// LATER avoid unnecessary copying
		char *buf = (char *)RTAlloc(unit->mWorld, len);
		if (buf) {
			args->getb(buf, len);
			CAST_UNIT->sendProgramData(totalSize, onset, buf, len);
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
	CHECK_UNIT;
	int count = args->geti();
	CAST_UNIT->receiveProgramData(count);
}

void vst_bank_read(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char *path = args->gets();
	if (path) {
		CAST_UNIT->readBank(path);
	}
	else {
		LOG_WARNING("vst_bank_read: expecting string argument!");
	}
}

void vst_bank_write(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char *path = args->gets();
	if (path) {
		CAST_UNIT->writeBank(path);
	}
	else {
		LOG_WARNING("vst_bank_write: expecting string argument!");
	}
}

void vst_bank_data_set(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int totalSize = args->geti();
	int onset = args->geti();
	int len = args->getbsize();
	if (len > 0) {
		// LATER avoid unnecessary copying
		char *buf = (char *)RTAlloc(unit->mWorld, len);
		if (buf) {
			args->getb(buf, len);
			CAST_UNIT->sendBankData(totalSize, onset, buf, len);
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
	CHECK_UNIT;
	int count = args->geti();
	CAST_UNIT->receiveBankData(count);
}


void vst_midi_msg(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	char data[4];
	int32 len = args->getbsize();
	if (len > 4) {
		LOG_WARNING("vst_midi_msg: midi message too long (" << len << " bytes)");
	}
	args->getb(data, len);
	CAST_UNIT->sendMidiMsg(data[0], data[1], data[2]);
}

void vst_midi_sysex(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int len = args->getbsize();
	if (len > 0) {
		// LATER avoid unnecessary copying
		char *buf = (char *)RTAlloc(unit->mWorld, len);
		if (buf) {
			args->getb(buf, len);
			CAST_UNIT->sendSysexMsg(buf, len);
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
	CHECK_UNIT;
	float bpm = args->getf();
	CAST_UNIT->setTempo(bpm);
}

void vst_time_sig(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 num = args->geti();
	int32 denom = args->geti();
	CAST_UNIT->setTimeSig(num, denom);
}

void vst_transport_play(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int play = args->geti();
	CAST_UNIT->setTransportPlaying(play);
}

void vst_transport_set(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	float pos = args->getf();
	CAST_UNIT->setTransportPos(pos);
}

void vst_transport_get(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	CAST_UNIT->getTransportPos();
}

void vst_can_do(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	const char* what = args->gets();
	if (what) {
		CAST_UNIT->canDo(what);
	}
}

void vst_vendor_method(Unit *unit, sc_msg_iter *args) {
	CHECK_UNIT;
	int32 index = args->geti();
	int32 value = args->geti(); // sc_msg_iter doesn't support 64bit ints...
	int32 size = args->getbsize();
	char *data = nullptr;
	if (size > 0) {
		data = (char *)RTAlloc(unit->mWorld, size);
		if (data) {
			args->getb(data, size);
		}
		else {
			LOG_ERROR("RTAlloc failed!");
			return;
		}
	}
	float opt = args->getf();
	CAST_UNIT->vendorSpecific(index, value, data, opt);
	if (data) {
		RTFree(unit->mWorld, data);
	}
}

/*** plugin command callbacks ***/

// add a user search path
void vst_path_add(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
	// LATER make this realtime safe (for now let's assume allocating some strings isn't a big deal)
	if (!isSearching) {
		while (args->remain() > 0) {
			auto path = args->gets();
			if (path) {
				userSearchPaths.push_back(path);
			}
		}
	}
	else {
		LOG_WARNING("currently searching!");
	}
}
// clear all user search paths
void vst_path_clear(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
	if (!isSearching) {
		userSearchPaths.clear();
	}
	else {
		LOG_WARNING("currently searching!");
	}
}
// scan for plugins on the Server
bool probePlugin(const std::string& fullPath, const std::string& key, bool verbose = false) {
	if (verbose) {
		Print("probing '%s' ... ", key.c_str());
	}
	auto plugin = loadVSTPlugin(fullPath, true);
	if (plugin) {
		auto& info = pluginMap[key];
		info.name = plugin->getPluginName();
		info.key = key;
		info.fullPath = fullPath;
		info.version = plugin->getPluginVersion();
		info.id = plugin->getPluginUniqueID();
		info.numInputs = plugin->getNumInputs();
		info.numOutputs = plugin->getNumOutputs();
		int numParameters = plugin->getNumParameters();
		info.parameters.clear();
		for (int i = 0; i < numParameters; ++i) {
			info.parameters.emplace_back(plugin->getParameterName(i), plugin->getParameterLabel(i));
		}
		int numPrograms = plugin->getNumPrograms();
		info.programs.clear();
		for (int i = 0; i < numPrograms; ++i) {
			auto pgm = plugin->getProgramNameIndexed(i);
			if (pgm.empty()) {
				plugin->setProgram(i);
				pgm = plugin->getProgramName();
			}
			info.programs.push_back(pgm);
		}
		uint32 flags = 0;
		flags |= plugin->hasEditor() << HasEditor;
		flags |= plugin->isSynth() << IsSynth;
		flags |= plugin->hasPrecision(VSTProcessPrecision::Single) << SinglePrecision;
		flags |= plugin->hasPrecision(VSTProcessPrecision::Double) << DoublePrecision;
		flags |= plugin->hasMidiInput() << MidiInput;
		flags |= plugin->hasMidiOutput() << MidiOutput;
		info.flags = flags;

		freeVSTPlugin(plugin);
		if (verbose) {
			Print("ok!\n");
		}
	}
	else {
		if (verbose) {
			Print("failed!\n");
		}
	}
	return plugin != nullptr;
}

bool cmdSearch(World *inWorld, void* cmdData) {
	auto data = (QueryCmdData *)cmdData;
	bool verbose = data->index;
	// search paths
	std::vector<std::string> searchPaths;
	// use defaults?
	if (data->value) {
		auto it = defaultSearchPaths;
		while (*it) searchPaths.push_back(*it++);
	}
	searchPaths.insert(searchPaths.end(), userSearchPaths.begin(), userSearchPaths.end());
	// extensions
	std::set<std::string> extensions;
	auto e = platformExtensions;
	while (*e) extensions.insert(*e++);
	// search recursively
	for (auto& path : searchPaths) {
		int count = 0;
		LOG_VERBOSE("searching in " << path << "...");
#ifdef _WIN32
		// root will have a trailing slash
		auto root = fs::absolute(fs::path(path)).string();
		for (auto& entry : fs::recursive_directory_iterator(root)) {
			if (fs::is_regular_file(entry)) {
				auto ext = entry.path().extension().string();
				if (extensions.count(ext)) {
					auto fullPath = entry.path().string();
					// shortPath: fullPath minus root minus extension (without leading slash)
					auto shortPath = fullPath.substr(root.size() + 1, fullPath.size() - root.size() - ext.size() - 1);
					// only forward slashes
					for (auto& c : shortPath) {
						if (c == '\\') c = '/';
					}
					if (!pluginMap.count(shortPath)) {
						// only probe plugin if hasn't been added to the map yet
						if (probePlugin(fullPath, shortPath, verbose)) count++;
					}
					else {
						count++;
					}
				}
			}
		}
#else // Unix
		// force trailing slash
		auto root = (path.back() != '/') ? path + "/" : path;
		std::function<void(const std::string&)> searchDir = [&](const std::string& dirname) {
			DIR *dir;
			struct dirent *entry;
			if ((dir = opendir(dirname.c_str()))) {
				while ((entry = readdir(dir)) != NULL) {
					std::string name(entry->d_name);
					std::string fullPath = dirname + "/" + name;
					if (entry->d_type == DT_DIR) {
						if (strcmp(name.c_str(), ".") != 0 && strcmp(name.c_str(), "..") != 0) {
							searchDir(fullPath);
						}
					}
					else {
						std::string ext;
						auto extPos = name.find('.');
						if (extPos != std::string::npos) {
							ext = name.substr(extPos);
						}
						if (extensions.count(ext)) {
							// shortPath: fullPath minus root minus extension (without leading slash)
							auto shortPath = fullPath.substr(root.size() + 1, fullPath.size() - root.size() - ext.size() - 1);
							if (!pluginMap.count(shortPath)) {
								// only probe plugin if hasn't been added to the map yet
								if (probePlugin(fullPath, shortPath, verbose)) count++;
							}
							else {
								count++;
							}
						}
					}
				}
				closedir(dir);
			}
		};
		searchDir(root);
#endif
		if (verbose) {
			LOG_VERBOSE("found " << count << " plugins.");
		}
	}
	// make list of plugin keys (so plugins can be queried by index)
	pluginList.clear();
	for (auto& entry : pluginMap) {
		pluginList.push_back(entry.first);
	}
	// report the number of plugins
	makeReply(data->reply, sizeof(data->reply), "/vst_search", (int)pluginList.size());
	// write to file (only for local Servers)
	std::string fileName(data->buf);
	if (fileName.size()) {
		LOG_DEBUG("writing plugin info file");
		// TODO
	}
	return true;
}

bool cmdSearchDone(World *inWorld, void *cmdData) {
	isSearching = false;
	// LOG_DEBUG("search done!");
	return true;
}

void vst_search(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
	if (isSearching) {
		LOG_WARNING("already searching!");
		return;
	}
	auto useDefault = args->geti();
	auto verbose = args->geti();
	auto path = args->gets();
	auto pathLen = path ? strlen(path) + 1 : 0; // extra char for 0
	auto data = (QueryCmdData *)RTAlloc(inWorld, sizeof(QueryCmdData) + pathLen);
	if (data) {
		isSearching = true;
		data->value = useDefault;
		data->index = verbose;
		if (path) memcpy(data->buf, path, pathLen);
		else data->buf[0] = 0;
		// LOG_DEBUG("start search");
		// set 'cmdName' inside stage2 (/vst_search + numPlugins)
		DoAsynchronousCommand(inWorld, replyAddr, data->reply, data, cmdSearch, cmdSearchDone, 0, cmdRTfree, 0, 0);
	}
	else {
		LOG_ERROR("RTAlloc failed!");
	}
}
// query plugin info
bool cmdQuery(World *inWorld, void *cmdData) {
	auto data = (QueryCmdData *)cmdData;
	bool verbose = data->value;
	std::string key;
	// query by path (probe if necessary)
	if (data->buf[0]) {
		key.assign(data->buf);
		if (!pluginMap.count(key)) {
			probePlugin(key, key); // key == path!
		}
	}
	// by index (already probed)
	else {
		int index = data->index;
		if (index >= 0 && index < pluginList.size()) {
			key = pluginList[index];
		}
	}
	// reply with plugin info
	auto it = pluginMap.find(key);
	if (it != pluginMap.end()) {
		auto& info = it->second;
		makeReply(data->reply, sizeof(data->reply), "/vst_info", key,
			info.name, info.fullPath, info.version, info.id, info.numInputs, info.numOutputs,
			(int)info.parameters.size(), (int)info.programs.size(), (int)info.flags);
	}
	else {
		// empty reply
		makeReply(data->reply, sizeof(data->reply), "/vst_info");
	}
	return true;
}

void vst_query(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
	if (isSearching) {
		LOG_WARNING("currently searching!");
		return;
	}
	QueryCmdData *data = nullptr;
	if (args->nextTag() == 's') {
		auto s = args->gets();
		auto size = strlen(s) + 1;
		data = (QueryCmdData *)RTAlloc(inWorld, sizeof(QueryCmdData) + size);
		if (data) {
			data->index = -1;
			memcpy(data->buf, s, size);
		}
		else {
			LOG_ERROR("RTAlloc failed!");
			return;
		}
	}
	else {
		data = (QueryCmdData *)RTAlloc(inWorld, sizeof(QueryCmdData));
		if (data) {
			data->index = args->geti();
			data->buf[0] = 0;
		}
		else {
			LOG_ERROR("RTAlloc failed!");
			return;
		}
	}
	// set 'cmdName' inside stage2 (/vst_info + info...)
	DoAsynchronousCommand(inWorld, replyAddr, data->reply, data, cmdQuery, 0, 0, cmdRTfree, 0, 0);
}

// query plugin parameter info
bool cmdQueryParam(World *inWorld, void *cmdData) {
	auto data = (QueryCmdData *)cmdData;
	auto it = pluginMap.find(data->buf);
	if (it != pluginMap.end()) {
		auto& params = it->second.parameters;
		auto reply = data->reply;
		int count = 0;
		int onset = sc_clip(data->index, 0, params.size());
		int num = sc_clip(data->value, 0, params.size() - onset);
		int size = sizeof(data->reply);
		count += doAddArg(reply, size, "/vst_param_info");
		// add plugin key (no need to check for overflow)
		count += doAddArg(reply + count, size - count, it->first);
		for (int i = 0; i < num && count < size; ++i) {
			auto& param = params[i + onset];
			count += doAddArg(reply + count, size - count, param.first); // name
			if (count < size) {
				count += doAddArg(reply + count, size - count, param.second); // label
			}
		}
		reply[count - 1] = '\0'; // remove trailing newline
	}
	else {
		// empty reply
		makeReply(data->reply, sizeof(data->reply), "/vst_param_info");
	}
	return true;
}

void vst_query_param(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
	if (isSearching) {
		LOG_WARNING("currently searching!");
		return;
	}
	auto key = args->gets();
	auto size = strlen(key) + 1;
	auto data = (QueryCmdData *)RTAlloc(inWorld, sizeof(QueryCmdData) + size);
	if (data) {
		data->index = args->geti(); // parameter onset
		data->value = args->geti(); // num parameters to query
		memcpy(data->buf, key, size);
		// set 'cmdName' inside stage2 (/vst_param_info + param names/labels...)
		DoAsynchronousCommand(inWorld, replyAddr, data->reply, data, cmdQueryParam, 0, 0, cmdRTfree, 0, 0);
	}
	else {
		LOG_ERROR("RTAlloc failed!");
	}
}

// query plugin default program info
bool cmdQueryProgram(World *inWorld, void *cmdData) {
	auto data = (QueryCmdData *)cmdData;
	auto it = pluginMap.find(data->buf);
	if (it != pluginMap.end()) {
		auto& programs = it->second.programs;
		auto reply = data->reply;
		int count = 0;
		int onset = sc_clip(data->index, 0, programs.size());
		int num = sc_clip(data->value, 0, programs.size() - onset);
		int size = sizeof(data->reply);
		count += doAddArg(reply, size, "/vst_program_info");
		// add plugin key (no need to check for overflow)
		count += doAddArg(reply + count, size - count, it->first);
		for (int i = 0; i < num && count < size; ++i) {
			count += doAddArg(reply + count, size - count, programs[i + onset]); // name
		}
		reply[count - 1] = '\0'; // remove trailing newline
	}
	else {
		// empty reply
		makeReply(data->reply, sizeof(data->reply), "/vst_program_info");
	}

	return true;
}

void vst_query_program(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
	if (isSearching) {
		LOG_WARNING("currently searching!");
		return;
	}
	auto key = args->gets();
	auto size = strlen(key) + 1;
	auto data = (QueryCmdData *)RTAlloc(inWorld, sizeof(QueryCmdData) + size);
	if (data) {
		data->index = args->geti(); // program onset
		data->value = args->geti(); // num programs to query
		memcpy(data->buf, key, size);
		// set 'cmdName' inside stage2 (/vst_param_info + param names/labels...)
		DoAsynchronousCommand(inWorld, replyAddr, data->reply, data, cmdQueryProgram, 0, 0, cmdRTfree, 0, 0);
	}
	else {
		LOG_ERROR("RTAlloc failed!");
	}
}

/*** plugin entry point ***/

void VstPlugin_Ctor(VstPlugin* unit){
	new(unit)VstPlugin();
}

void VstPlugin_Dtor(VstPlugin* unit){
	unit->~VstPlugin();
}

#define UnitCmd(x) DefineUnitCmd("VstPlugin", "/" #x, vst_##x)
#define PluginCmd(x) DefinePlugInCmd("/" #x, x, 0)

PluginLoad(VstPlugin) {
    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable
	DefineDtorCantAliasUnit(VstPlugin);
	UnitCmd(open);
	UnitCmd(close);
	UnitCmd(reset);
	UnitCmd(vis);
	UnitCmd(set);
	UnitCmd(setn);
	UnitCmd(param_query);
	UnitCmd(get);
	UnitCmd(getn);
	UnitCmd(map);
	UnitCmd(unmap);
	UnitCmd(program_set);
	UnitCmd(program_query);
	UnitCmd(program_name);
	UnitCmd(program_read);
	UnitCmd(program_write);
	UnitCmd(program_data_set);
	UnitCmd(program_data_get);
	UnitCmd(bank_read);
	UnitCmd(bank_write);
	UnitCmd(bank_data_set);
	UnitCmd(bank_data_get);
	UnitCmd(midi_msg);
	UnitCmd(midi_sysex);
	UnitCmd(tempo);
	UnitCmd(time_sig);
	UnitCmd(transport_play);
	UnitCmd(transport_set);
	UnitCmd(transport_get);
	UnitCmd(can_do);
	UnitCmd(vendor_method);

    PluginCmd(vst_search);
	PluginCmd(vst_query);
	PluginCmd(vst_query_param);
	PluginCmd(vst_query_program);
	PluginCmd(vst_path_add);
	PluginCmd(vst_path_clear);
}
