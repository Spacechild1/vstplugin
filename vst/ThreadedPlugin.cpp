#include "ThreadedPlugin.h"

#include <string.h>
#include <assert.h>

#if 0
#define THREAD_DEBUG(x) LOG_DEBUG(x)
#else
#define THREAD_DEBUG(x)
#endif

namespace vst {

/*/////////////////// DSPThreadPool /////////////////////////*/

// there's a deadlock bug in the windows runtime library which would cause
// the process to hang if trying to join a thread in a static object destructor.
#ifdef _WIN32
# define DSPTHREADPOOL_JOIN 0
#else
# define DSPTHREADPOOL_JOIN 1
#endif

DSPThreadPool::DSPThreadPool() {
#if 0
    THREAD_DEBUG("Align of DSPThreadPool: " << alignof(*this));
    THREAD_DEBUG("DSPThreadPool address: " << this);
    THREAD_DEBUG("pushLock address: " << &pushLock_);
    THREAD_DEBUG("popLock address: " << &popLock_);
#endif

    running_.store(true);

    //  number of available hardware threads minus one (= the main audio thread)
    int numThreads = std::max<int>(std::thread::hardware_concurrency() - 1, 1);
    THREAD_DEBUG("number of DSP helper threads: " << numThreads);

    for (int i = 0; i < numThreads; ++i){
        std::thread thread([this, i](){
            setThreadPriority(Priority::High);
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
                event_.wait();

                THREAD_DEBUG("DSP thread " << i << " woke up");
            }
        });
    #if !DSPTHREADPOOL_JOIN
        thread.detach();
    #endif
        threads_.push_back(std::move(thread));
    }
}

DSPThreadPool::~DSPThreadPool(){
#if DSPTHREADPOOL_JOIN
    running_.store(false);

    // wake up all threads!
    auto n = threads_.size();
    while (n--){
        event_.set();
    }
    // join threads
    for (auto& thread : threads_){
        if (thread.joinable()){
            thread.join();
        }
    }
#endif
}

bool DSPThreadPool::push(Callback cb, ThreadedPlugin *plugin, int numSamples){
    pushLock_.lock();
    bool result = queue_.push({ cb, plugin, numSamples });
    pushLock_.unlock();
    THREAD_DEBUG("DSPThreadPool::push");
    event_.set();
    return result;
}

/*////////////////////// ThreadedPlugin ///////////////////////*/

IPlugin::ptr makeThreadedPlugin(IPlugin::ptr plugin){
    return std::make_unique<ThreadedPlugin>(std::move(plugin));
}

ThreadedPlugin::ThreadedPlugin(IPlugin::ptr plugin)
    : plugin_(std::move(plugin)) {
    threadPool_ = &DSPThreadPool::instance(); // cache for performance
    event_.set(); // so that the process routine doesn't wait the very first time
}

ThreadedPlugin::~ThreadedPlugin() {
    // wait for last processing to finish (ideally we shouldn't have to)
    event_.wait();
    // avoid memleak with param string and sysex command
    for (int i = 0; i < 2; ++i){
        for (auto& cmd : commands_[i]){
            if (cmd.type == Command::SetParamString){
                delete[] cmd.paramString.display;
            } else if (cmd.type == Command::SendSysex){
                delete[] cmd.sysex.data;
            }
        }
    }
}

void ThreadedPlugin::setListener(IPluginListener::ptr listener){
    listener_ = listener;
    if (listener){
        auto proxy = std::make_shared<ThreadedPluginListener>(*this);
        proxyListener_ = proxy; // keep alive
        plugin_->setListener(proxy);
    } else {
        plugin_->setListener(nullptr);
        proxyListener_ = nullptr;
    }
}

void ThreadedPlugin::setupProcessing(double sampleRate, int maxBlockSize, ProcessPrecision precision) {
    ScopedLock lock(mutex_);
    plugin_->setupProcessing(sampleRate, maxBlockSize, precision);

    if (maxBlockSize != blockSize_ || precision != precision_){
        blockSize_ = maxBlockSize;
        precision_ = precision;

        updateBuffer();
    }
}

void ThreadedPlugin::updateBuffer(){
    int total = input_.size() + auxInput_.size() + output_.size() + auxOutput_.size();
    const int incr = blockSize_ *
            (precision_ == ProcessPrecision::Double ? sizeof(double) : sizeof(float));
    buffer_.clear(); // force zero
    buffer_.resize(total * incr);
    // set buffer vectors
    auto buffer = buffer_.data();
    for (size_t i = 0; i < input_.size(); ++i, buffer += incr){
        input_[i] = (void *)buffer;
    }
    for (size_t i = 0; i < auxInput_.size(); ++i, buffer += incr){
        auxInput_[i] = (void *)buffer;
    }
    for (size_t i = 0; i < output_.size(); ++i, buffer += incr){
        output_[i] = (void *)buffer;
    }
    for (size_t i = 0; i < auxOutput_.size(); ++i, buffer += incr){
        auxOutput_[i] = (void *)buffer;
    }
    assert((buffer - buffer_.data()) == buffer_.size());
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
            plugin_->setParameter(command.paramString.index, command.paramString.display,
                                  command.paramString.offset);
            delete[] command.paramString.display; // !
            break;
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
            LOG_ERROR("bug: unknown command in ThreadedPlugin");
            break;
        }
    }
    // clear queue!
    commands_[!current_].clear();
}

template<typename T>
void ThreadedPlugin::threadFunction(int numSamples){
    if (mutex_.try_lock()){
        // set thread ID!
        rtThread_ = std::this_thread::get_id();

        // clear outgoing event queue!
        events_[!current_].clear();

        dispatchCommands();

        ProcessData<T> data;
        data.numSamples = numSamples;
        data.input = (const T **)input_.data();
        data.numInputs = (int)input_.size();
        data.auxInput = (const T **)auxInput_.data();
        data.numAuxInputs = (int)auxInput_.size();
        data.output = (T **)output_.data();
        data.numOutputs = (int)output_.size();
        data.auxOutput = (T **)auxOutput_.data();
        data.numAuxOutputs = (int)auxOutput_.size();

        plugin_->process(data);
        mutex_.unlock();
    } else {
        // copy input to output
        auto bypass = [](auto& input, auto& output, int blocksize){
            for (size_t i = 0; i < output.size(); ++i){
                if (i < input.size()){
                    std::copy((T *)input[i], (T *)input[i] + blocksize, (T *)output[i]);
                } else {
                    std::fill((T *)output[i], (T *)output[i] + blocksize, 0);
                }
            }
        };
        bypass(input_, output_, numSamples);
        bypass(auxInput_, auxOutput_, numSamples);
        LOG_DEBUG("couldn't lock mutex - bypassing");
    }

    event_.set();
}

template<typename T>
void ThreadedPlugin::doProcess(ProcessData<T>& data){
    // LATER do *hard* bypass here and not in the thread function

    // wait for last processing to finish (ideally we shouldn't have to)
    event_.wait();
    // get new input from host
    assert(data.numInputs == input_.size());
    for (int i = 0; i < data.numInputs; ++i){
        std::copy(data.input[i], data.input[i] + data.numSamples, (T *)input_[i]);
    }
    assert(data.numAuxInputs == auxInput_.size());
    for (int i = 0; i < data.numAuxInputs; ++i){
        std::copy(data.auxInput[i], data.auxInput[i] + data.numSamples, (T *)auxInput_[i]);
    }
    // send last output to host
    assert(data.numOutputs == output_.size());
    auto output = (T **)output_.data();
    for (int i = 0; i < data.numOutputs; ++i){
        std::copy(output[i], output[i] + data.numSamples, data.output[i]);
    }
    assert(data.numAuxOutputs == auxOutput_.size());
    auto auxOutput = (T **)auxOutput_.data();
    for (int i = 0; i < data.numAuxOutputs; ++i){
        std::copy(auxOutput[i], auxOutput[i] + data.numSamples, data.auxOutput[i]);
    }
    // swap queues and notify DSP thread pool
    current_ = !current_;
    auto cb = [](ThreadedPlugin *plugin, int numSamples){
        plugin->threadFunction<T>(numSamples);
    };
    if (!threadPool_->push(cb, this, data.numSamples)){
        LOG_WARNING("couldn't push DSP task!");
        // skip processing and clear outputs
        for (int i = 0; i < data.numOutputs; ++i){
            std::fill(output[i], output[i] + data.numSamples, 0);
        }
        for (int i = 0; i < data.numAuxOutputs; ++i){
            std::fill(auxOutput[i], auxOutput[i] + data.numSamples, 0);
        }
        event_.set(); // so that the next call to event_.wait() doesn't block!
    }

    sendEvents();
}

void ThreadedPlugin::sendEvents(){
    auto listener = listener_.lock();
    if (listener){
        for (auto& event : events_[current_]){
            switch (event.type){
            case Command::ParamAutomated:
                listener->parameterAutomated(event.paramAutomated.index,
                                             event.paramAutomated.value);
                break;
            case Command::LatencyChanged:
                listener->latencyChanged(event.i);
                break;
            case Command::MidiReceived:
                listener->midiEvent(event.midi);
                break;
            case Command::SysexReceived:
                listener->sysexEvent(event.sysex);
                delete[] event.sysex.data; // delete data!
                break;
            default:
                break;
            }
        }
    }
}

void ThreadedPlugin::process(ProcessData<float>& data) {
    doProcess(data);
}

void ThreadedPlugin::process(ProcessData<double>& data) {
    doProcess(data);
}

void ThreadedPlugin::suspend() {
    ScopedLock lock(mutex_);
    plugin_->suspend();
}

void ThreadedPlugin::resume() {
    ScopedLock lock(mutex_);
    plugin_->resume();
}

void ThreadedPlugin::setNumSpeakers(int in, int out, int auxIn, int auxOut) {
    ScopedLock lock(mutex_);
    plugin_->setNumSpeakers(in, out, auxIn, auxOut);
    // floatData and doubleData have the same layout
    input_.resize(in);
    auxInput_.resize(auxIn);
    output_.resize(out);
    auxOutput_.resize(auxOut);

    updateBuffer();
}

float ThreadedPlugin::getParameter(int index) const {
    // this should be threadsafe, but we might read an old value.
    // we can't set a parameter and immediately retrieve it,
    // instead we need one block of delay.
    return plugin_->getParameter(index);
}

std::string ThreadedPlugin::getParameterString(int index) const {
    // see above
    return plugin_->getParameterString(index);
}

void ThreadedPlugin::setProgramName(const std::string& name) {
    ScopedLock lock(mutex_);
    plugin_->setProgramName(name);
}

int ThreadedPlugin::getProgram() const {
    return plugin_->getProgram();
}

std::string ThreadedPlugin::getProgramName() const {
    // LATER improve
    ScopedLock lock(mutex_);
    return plugin_->getProgramName();
}

std::string ThreadedPlugin::getProgramNameIndexed(int index) const {
    // LATER improve
    ScopedLock lock(mutex_);
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
    ScopedLock lock(mutex_);
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
    ScopedLock lock(mutex_);
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
    ScopedLock lock(mutex_);
    plugin_->readBankData(data, size);
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
    ScopedLock lock(mutex_);
    plugin_->writeBankData(buffer);
}

intptr_t ThreadedPlugin::vendorSpecific(int index, intptr_t value, void *p, float opt) {
    ScopedLock lock(mutex_);
    return plugin_->vendorSpecific(index, value, p, opt);
}

/*/////////////////// ThreadedPluginListener ////////////////////*/

void ThreadedPluginListener::parameterAutomated(int index, float value) {
    if (std::this_thread::get_id() == owner_->rtThread_){
        Command e(Command::ParamAutomated);
        e.paramAutomated.index = index;
        e.paramAutomated.value = value;

        owner_->pushEvent(e);
    } else {
        // UI or NRT thread
        auto listener = owner_->listener_.lock();
        if (listener){
            listener->parameterAutomated(index, value);
        }
    }
}

void ThreadedPluginListener::latencyChanged(int nsamples) {
    if (std::this_thread::get_id() == owner_->rtThread_){
        Command e(Command::LatencyChanged);
        e.i = nsamples;

        owner_->pushEvent(e);
    } else {
        // UI or NRT thread
        auto listener = owner_->listener_.lock();
        if (listener){
            listener->latencyChanged(nsamples);
        }
    }
}

void ThreadedPluginListener::pluginCrashed(){
    // UI or NRT thread
    auto listener = owner_->listener_.lock();
    if (listener){
        listener->pluginCrashed();
    }
}

void ThreadedPluginListener::midiEvent(const MidiEvent& event) {
    if (std::this_thread::get_id() == owner_->rtThread_){
        Command e(Command::MidiReceived);
        e.midi = event;

        owner_->pushEvent(e);
    } else {
        // UI or NRT thread
        auto listener = owner_->listener_.lock();
        if (listener){
            listener->midiEvent(event);
        }
    }
}

void ThreadedPluginListener::sysexEvent(const SysexEvent& event) {
    if (std::this_thread::get_id() == owner_->rtThread_){
        // deep copy!
        auto data = new char[event.size];
        memcpy(data, event.data, event.size);

        Command e(Command::SysexReceived);
        e.sysex.data = data;
        e.sysex.size = event.size;
        e.sysex.delta = event.delta;

        owner_->pushEvent(e);
    } else {
        // UI or NRT thread
        auto listener = owner_->listener_.lock();
        if (listener){
            listener->sysexEvent(event);
        }
    }
}

} // vst
