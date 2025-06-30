#include "ThreadedPlugin.h"

#include "Log.h"
#include "MiscUtils.h"

#include <string.h>
#include <fstream>
#include <assert.h>

#ifndef DEBUG_THREADPOOL
#define DEBUG_THREADPOOL 0
#endif

#if DEBUG_THREADPOOL
#define THREAD_DEBUG(x) LOG_DEBUG(x)
#else
#define THREAD_DEBUG(x)
#endif

namespace vst {

/*/////////////////// DSPThreadPool /////////////////////////*/

static std::atomic<int> gNumDSPThreads;

int getNumLogicalCPUs() {
    static auto n = std::thread::hardware_concurrency();
    return n;
}

// set the number of DSP threads (0 = default)
void setNumDSPThreads(int numThreads) {
    LOG_DEBUG("setNumDSPThreads: " << numThreads);
    gNumDSPThreads.store(std::max<int>(numThreads, 0));
}

int getNumDSPThreads() {
    auto numThreads = gNumDSPThreads.load();
    if (numThreads > 0) {
        return numThreads;
    } else {
        return getNumLogicalCPUs(); // default
    }
}

static thread_local bool gCurrentThreadDSP;

// some callbacks in IPluginListener need to know whether they are
// called from a DSP (helper) thread, so that they would push to
// a queue instead of forwarding to the "real" listener.
// This is simpler and faster than saving and checking thread IDs.
void setCurrentThreadDSP() {
    gCurrentThreadDSP = true;
}

bool isCurrentThreadDSP() {
    // LOG_DEBUG("isCurrentThreadDSP: " << gCurrentThreadDSP);
    return gCurrentThreadDSP;
}

DSPThreadPool::DSPThreadPool() {
    LOG_DEBUG("start DSPThreadPool");
#if 0
    THREAD_DEBUG("Align of DSPThreadPool: " << alignof(*this));
    THREAD_DEBUG("DSPThreadPool address: " << this);
    THREAD_DEBUG("pushLock address: " << &pushLock_);
    THREAD_DEBUG("popLock address: " << &popLock_);
#endif

    running_.store(true);

    //  number of available hardware threads minus one (= the main audio thread)
    int numThreads = std::max<int>(getNumDSPThreads() - 1, 1);
    THREAD_DEBUG("number of DSP helper threads: " << numThreads);

    for (int i = 0; i < numThreads; ++i){
        std::thread thread([this, i](){
            setThreadPriority(Priority::High);
            setCurrentThreadDSP();
            run(i);
        });
        threads_.push_back(std::move(thread));
    }
}

DSPThreadPool::~DSPThreadPool(){
#ifdef _WIN32
    // You can't synchronize threads in a global/static object
    // destructor in a Windows DLL because of the loader lock.
    // See https://docs.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices
    for (auto& thread : threads_){
        thread.detach();
    }
#else
    running_.store(false);

    // wake up all threads!
    semaphore_.post(threads_.size());
    // join threads
    for (auto& thread : threads_){
        if (thread.joinable()){
            thread.join();
        }
    }
#endif
    LOG_DEBUG("free DSPThreadPool");
}

bool DSPThreadPool::push(Callback cb, ThreadedPlugin *plugin, int numSamples){
    pushLock_.lock();
    bool result = queue_.push({ cb, plugin, numSamples });
    pushLock_.unlock();
    THREAD_DEBUG("DSPThreadPool: push task");
    semaphore_.post();
    return result;
}

bool DSPThreadPool::processTask(){
    Task task;
    popLock_.lock();
    bool result = queue_.pop(task);
    popLock_.unlock();
    if (result) {
        // call DSP routine
        task.cb(task.plugin, task.numSamples);
    }
    return result;
}

void DSPThreadPool::run(int index) {
    // the loop
    while (running_.load()) {
        Task task;
        popLock_.lock();
        while (queue_.pop(task)){
            popLock_.unlock();
            // call DSP routine
            task.cb(task.plugin, task.numSamples);
            popLock_.lock();
        }
        popLock_.unlock();

        // wait for more
        semaphore_.wait();

        THREAD_DEBUG("DSP helper thread " << index << " woke up");
    }
}

/*////////////////////// ThreadedPlugin ///////////////////////*/

IPlugin::ptr createThreadedPlugin(IPlugin::ptr plugin){
    return std::make_unique<ThreadedPlugin>(std::move(plugin));
}

ThreadedPlugin::ThreadedPlugin(IPlugin::ptr plugin)
    : plugin_(std::move(plugin)) {
    threadPool_ = &DSPThreadPool::instance(); // cache for performance
    event_.set(); // so that the process routine doesn't wait the very first time
    LOG_DEBUG("ThreadedPlugin");
}

ThreadedPlugin::~ThreadedPlugin() {
    // just to be sure
    plugin_->setListener(nullptr);
    // wait for last processing to finish (ideally we shouldn't have to)
    event_.wait();
    // avoid memleak with param string and sysex command
    for (int i = 0; i < 2; ++i){
        for (auto& cmd : commands_[i]){
            if (cmd.type == Command::SetParamString){
                delete[] cmd.paramString.str;
            } else if (cmd.type == Command::SendSysex){
                delete[] cmd.sysex.data;
            }
        }
    }
}

void ThreadedPlugin::setListener(IPluginListener* listener){
    listener_ = listener;
    if (listener){
        plugin_->setListener(this);
    } else {
        plugin_->setListener(nullptr);
    }
}

void ThreadedPlugin::setupProcessing(double sampleRate, int maxBlockSize,
                                     ProcessPrecision precision, ProcessMode mode) {
    std::lock_guard lock(mutex_);
    plugin_->setupProcessing(sampleRate, maxBlockSize, precision, mode);

    if (maxBlockSize != blockSize_ || precision != precision_){
        blockSize_ = maxBlockSize;
        precision_ = precision;
        mode_ = mode;

        updateBuffer();
    }
}

void ThreadedPlugin::updateBuffer(){
    int total = 0;
    for (int i = 0; i < numInputs_; ++i){
        total += inputs_[i].numChannels;
    }
    for (int i = 0; i < numOutputs_; ++i){
        total += outputs_[i].numChannels;
    }
    const int incr = blockSize_ *
        ((precision_ == ProcessPrecision::Double) ? sizeof(double) : sizeof(float));
    buffer_.clear(); // force zero initialization
    buffer_.resize(total * incr);
    // set buffer vectors
    auto setChannels = [](auto& bus, auto& buffer, int incr){
        for (int i = 0; i < bus.numChannels; ++i){
            bus.channelData32[i] = (float *)buffer; // float* and double* have the same size
            buffer += incr;
        }
    };
    auto buf = buffer_.data();
    for (int i = 0; i < numInputs_; ++i){
        setChannels(inputs_[i], buf, incr);
    }
    for (int i = 0; i < numOutputs_; ++i){
        setChannels(outputs_[i], buf, incr);
    }
    assert((buf - buffer_.data()) == buffer_.size());
}

void ThreadedPlugin::dispatchCommands() {
    // read last queue
    for (auto& command : commands_[!current_]){
        switch(command.type){
        case Command::SetParamValue:
            plugin_->setParameter(command.paramValue.index, command.paramValue.value,
                                  command.paramValue.offset);
            break;
        case Command::SetParamString:
            plugin_->setParameter(command.paramString.index, command.paramString.str,
                                  command.paramString.offset);
            delete[] command.paramString.str; // !
            break;
        case Command::SetParamStringShort:
        {
            auto& cmd = command.paramStringShort;
            assert(cmd.pstr[0] <= Command::maxShortStringSize);
            std::string str((char *)&cmd.pstr[1], cmd.pstr[0]);
            plugin_->setParameter(cmd.index, str, cmd.offset);
            break;
        }
        case Command::SetBypass:
            plugin_->setBypass(static_cast<Bypass>(command.i));
            break;
        case Command::SetTempo:
            plugin_->setTempoBPM(command.d);
            break;
        case Command::SetTimeSignature:
            plugin_->setTimeSignature(command.timeSig.num, command.timeSig.denom);
            break;
        case Command::SetTransportPlaying:
            plugin_->setTransportPlaying(command.i);
            break;
        case Command::SetTransportRecording:
            plugin_->setTransportRecording(command.i);
            break;
        case Command::SetTransportAutomationWriting:
            plugin_->setTransportAutomationWriting(command.i);
            break;
        case Command::SetTransportAutomationReading:
            plugin_->setTransportAutomationReading(command.i);
            break;
        case Command::SetTransportCycleActive:
            plugin_->setTransportCycleActive(command.i);
            break;
        case Command::SetTransportCycleStart:
            plugin_->setTransportCycleStart(command.d);
            break;
        case Command::SetTransportCycleEnd:
            plugin_->setTransportCycleEnd(command.d);
            break;
        case Command::SetTransportPosition:
            plugin_->setTransportPosition(command.d);
            break;
        case Command::SendMidi:
            plugin_->sendMidiEvent(command.midi);
            break;
        case Command::SendSysex:
            plugin_->sendSysexEvent(command.sysex);
            delete[] command.sysex.data; // !
            break;
        case Command::SetProgram:
            plugin_->setProgram(command.i);
            break;
        default:
            LOG_ERROR("ThreadedPlugin::dispatchCommands: unknown command");
            break;
        }
    }
    // clear queue!
    commands_[!current_].clear();
}

template<typename T>
void ThreadedPlugin::threadFunction(int numSamples){
    ProcessData data;
    data.precision = precision_;
    data.mode = mode_;
    data.numSamples = numSamples;
    data.inputs = inputs_.get();
    data.numInputs = numInputs_;
    data.outputs = outputs_.get();
    data.numOutputs = numOutputs_;

    if (mutex_.try_lock()){
        // clear outgoing event queue!
        events_[!current_].clear();

        dispatchCommands();

        plugin_->process(data);

        mutex_.unlock();
    } else {
        bypass(data);
        LOG_DEBUG("couldn't lock mutex - bypassing");
    }

    event_.set();
}

template<typename T>
void ThreadedPlugin::doProcess(ProcessData& data){
    // LATER do *hard* bypass here and not in the thread function

    // check event without blocking.
    // LOG_DEBUG("try to wait for task");
    if (!event_.try_wait()){
        setCurrentThreadDSP(); // !
        bool didWait = false;
        do {
            // instead of waiting, try to process a task.
            // NOTE: we only process a single task at a time and then check again,
            // because in the meantime another thread might have finished our task.
            // in this case, we can move on and let the DSP threads do the remaining work.
            if (!threadPool_->processTask()){
                if (!didWait) {
                    // LOG_DEBUG("wait for task");
                    didWait = true;
                }
                for (int count = 1000; count > 0; count--) {
                    pauseCpu();
                }
            } else {
                // LOG_DEBUG("process task");
            }
        } while (!event_.try_wait());
    }

    auto copyChannels = [](auto& from, auto& to, int nsamples){
        assert(from.numChannels == to.numChannels);
        for (int i = 0; i < from.numChannels; ++i){
            auto src = (T *)from.channelData32[i]; // cast to actual size
            auto dst = (T *)to.channelData32[i];
            std::copy(src, src + nsamples, dst);
        }
    };
    // get new input from host
    assert(data.numInputs == numInputs_);
    for (int i = 0; i < data.numInputs; ++i){
        copyChannels(data.inputs[i], inputs_[i], data.numSamples);
    }
    // send last output to host
    assert(data.numOutputs == numOutputs_);
    for (int i = 0; i < data.numOutputs; ++i){
        copyChannels(outputs_[i], data.outputs[i], data.numSamples);
    }
    // swap queues and notify DSP thread pool
    current_ = !current_;
    auto cb = [](ThreadedPlugin *plugin, int numSamples){
        plugin->threadFunction<T>(numSamples);
    };
    if (!threadPool_->push(cb, this, data.numSamples)){
        LOG_WARNING("ThreadedPlugin: couldn't push DSP task!");
        // skip processing and clear outputs
        for (int i = 0; i < numOutputs_; ++i){
            auto& output = outputs_[i];
            for (int j = 0; j < output.numChannels; ++j){
                auto chn = (T *)output.channelData32[j]; // cast to actual size
                std::fill(chn, chn + data.numSamples, 0);
            }
        }

        event_.set(); // so that the next call to event_.wait() doesn't block!
    }

    sendEvents();
}

void ThreadedPlugin::sendEvents(){
    if (listener_){
        for (auto& event : events_[current_]){
            switch (event.type){
            case Command::ParamAutomated:
                listener_->parameterAutomated(event.paramAutomated.index,
                                             event.paramAutomated.value);
                break;
            case Command::LatencyChanged:
                listener_->latencyChanged(event.i);
                break;
            case Command::MidiReceived:
                listener_->midiEvent(event.midi);
                break;
            case Command::SysexReceived:
                listener_->sysexEvent(event.sysex);
                delete[] event.sysex.data; // delete data!
                break;
            default:
                break;
            }
        }
    }
}

void ThreadedPlugin::process(ProcessData& data) {
    if (data.precision == ProcessPrecision::Double){
        doProcess<double>(data);
    } else {
        doProcess<float>(data);
    }
}

void ThreadedPlugin::suspend() {
    std::lock_guard lock(mutex_);
    plugin_->suspend();
}

void ThreadedPlugin::resume() {
    std::lock_guard lock(mutex_);
    plugin_->resume();
}

void ThreadedPlugin::setNumSpeakers(int *input, int numInputs,
                                    int *output, int numOutputs) {
    std::lock_guard lock(mutex_);
    plugin_->setNumSpeakers(input, numInputs, output, numOutputs);
    // create input busses
    inputs_ = numInputs > 0 ? std::make_unique<Bus[]>(numInputs) : nullptr;
    numInputs_ = numInputs;
    for (int i = 0; i < numInputs; ++i){
        inputs_[i] = Bus(input[i]);
    }
    // create output busses
    outputs_ = numOutputs > 0 ? std::make_unique<Bus[]>(numOutputs) : nullptr;
    numOutputs_ = numOutputs;
    for (int i = 0; i < numOutputs; ++i){
        outputs_[i] = Bus(output[i]);
    }

    updateBuffer();
}

float ThreadedPlugin::getParameter(int index) const {
    // This should be threadsafe, but we might read an old value.
    // We can't set a parameter and immediately retrieve it,
    // instead we need one block of delay.
    return plugin_->getParameter(index);
}

size_t ThreadedPlugin::getParameterString(int index, ParamStringBuffer& buffer) const {
    // see getParameter() above
    return plugin_->getParameterString(index, buffer);
}

void ThreadedPlugin::setProgram(int index) {
    // let's cache immediately
#if 1
    program_ = index;
#endif
    DeferredPlugin::setProgram(index);
}

int ThreadedPlugin::getProgram() const {
    // NB: is this thread-safe?
    return plugin_->getProgram();
}

void ThreadedPlugin::setProgramName(std::string_view name) {
    std::lock_guard lock(mutex_);
    plugin_->setProgramName(name);
}

std::string ThreadedPlugin::getProgramName() const {
    // LATER improve
    std::lock_guard lock(mutex_);
    return plugin_->getProgramName();
}

std::string ThreadedPlugin::getProgramNameIndexed(int index) const {
    // LATER improve
    std::lock_guard lock(mutex_);
    return plugin_->getProgramNameIndexed(index);
}

void ThreadedPlugin::readProgramFile(const std::string& path) {
    std::ifstream file(path, std::ios_base::binary);
    if (!file.is_open()){
        throw Error("couldn't open file " + path);
    }
    file.seekg(0, std::ios_base::end);
    std::string buffer;
    buffer.resize(file.tellg());
    file.seekg(0, std::ios_base::beg);
    file.read(&buffer[0], buffer.size());
    readProgramData(buffer.data(), buffer.size());
}

void ThreadedPlugin::readProgramData(const char *data, size_t size) {
    std::lock_guard lock(mutex_);
    plugin_->readProgramData(data, size);
}

void ThreadedPlugin::writeProgramFile(const std::string& path) {
    std::ofstream file(path, std::ios_base::binary | std::ios_base::trunc);
    if (!file.is_open()){
        throw Error("couldn't create file " + path);
    }
    std::string buffer;
    writeProgramData(buffer);
    file.write(buffer.data(), buffer.size());
}

void ThreadedPlugin::writeProgramData(std::string& buffer) {
    std::lock_guard lock(mutex_);
    plugin_->writeProgramData(buffer);
}

void ThreadedPlugin::readBankFile(const std::string& path) {
    std::ifstream file(path, std::ios_base::binary);
    if (!file.is_open()){
        throw Error("couldn't open file " + path);
    }
    file.seekg(0, std::ios_base::end);
    std::string buffer;
    buffer.resize(file.tellg());
    file.seekg(0, std::ios_base::beg);
    file.read(&buffer[0], buffer.size());
    readBankData(buffer.data(), buffer.size());
}

void ThreadedPlugin::readBankData(const char *data, size_t size) {
    std::lock_guard lock(mutex_);
    plugin_->readBankData(data, size);
    // update program number
    program_ = plugin_->getProgram();
}

void ThreadedPlugin::writeBankFile(const std::string& path) {
    std::ofstream file(path, std::ios_base::binary | std::ios_base::trunc);
    if (!file.is_open()){
        throw Error("couldn't create file " + path);
    }
    std::string buffer;
    writeBankData(buffer);
    file.write(buffer.data(), buffer.size());
}

void ThreadedPlugin::writeBankData(std::string& buffer) {
    std::lock_guard lock(mutex_);
    plugin_->writeBankData(buffer);
}

intptr_t ThreadedPlugin::vendorSpecific(int index, intptr_t value, void *p, float opt) {
    std::lock_guard lock(mutex_);
    return plugin_->vendorSpecific(index, value, p, opt);
}


void ThreadedPlugin::parameterAutomated(int index, float value) {
    if (isCurrentThreadDSP()) {
        Command e(Command::ParamAutomated);
        e.paramAutomated.index = index;
        e.paramAutomated.value = value;

        pushEvent(e);
    } else {
        // UI or NRT thread
        if (listener_){
            listener_->parameterAutomated(index, value);
        }
    }
}

void ThreadedPlugin::latencyChanged(int nsamples) {
    if (isCurrentThreadDSP()) {
        Command e(Command::LatencyChanged);
        e.i = nsamples;

        pushEvent(e);
    } else {
        // UI or NRT thread
        if (listener_){
            listener_->latencyChanged(nsamples);
        }
    }
}

void ThreadedPlugin::updateDisplay() {
    if (listener_){
        listener_->updateDisplay();
    }
}

void ThreadedPlugin::pluginCrashed(){
    // UI or NRT thread
    if (listener_){
        listener_->pluginCrashed();
    }
}

void ThreadedPlugin::midiEvent(const MidiEvent& event) {
    if (isCurrentThreadDSP()) {
        Command e(Command::MidiReceived);
        e.midi = event;

        pushEvent(e);
    } else {
        // UI or NRT thread
        if (listener_){
            listener_->midiEvent(event);
        }
    }
}

void ThreadedPlugin::sysexEvent(const SysexEvent& event) {
    if (isCurrentThreadDSP()) {
        // deep copy!
        auto data = new char[event.size];
        memcpy(data, event.data, event.size);

        Command e(Command::SysexReceived);
        e.sysex.data = data;
        e.sysex.size = event.size;
        e.sysex.delta = event.delta;

        pushEvent(e);
    } else {
        // UI or NRT thread
        if (listener_){
            listener_->sysexEvent(event);
        }
    }
}

} // vst
