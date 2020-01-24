#include "ThreadedPlugin.h"
#include "Utility.h"

#include <string.h>
#include <assert.h>
#include <thread>
#include <condition_variable>
#include <atomic>

namespace vst {

Event::Event() {
#if USE_PLATFORM_EVENT
#if defined(_WIN32)
    event_ = CreateEvent(nullptr, 0, 0, nullptr);
#elif defined(__APPLE__)
    sem_ = dispatch_semaphore_create(0);
#else // pthreads
    sem_init(&sem_, 0, 0);
#endif
#endif // USE_PLATFORM_EVENT
}

Event::~Event(){
#if USE_PLATFORM_EVENT
#if defined(_WIN32)
    CloseHandle(event_);
#elif defined(__APPLE__)
    dispatch_release(sem_);
#else // pthreads
    sem_destroy(&sem_);
#endif
#endif // USE_PLATFORM_EVENT
}

void Event::signal(){
#if USE_PLATFORM_EVENT
#if defined(_WIN32)
    SetEvent(event_);
#elif defined(__APPLE__)
    dispatch_semaphore_signal(sem_);
#else
    sem_post(&sem_);
#endif
#else // USE_PLATFORM_EVENT
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = true;
    condition_.notify_one();
#endif
}

void Event::wait(){
#if USE_PLATFORM_EVENT
#if defined(_WIN32)
    WaitForSingleObject(event_, INFINITE);
#elif defined(__APPLE__)
    dispatch_semaphore_wait(sem_, DISPATCH_TIME_FOREVER);
#else
    sem_wait(&sem_);
#endif
#else // USE_PLATFORM_EVENT
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [](){ return state_; });
    state_ = false;
#endif
}

/*/////////////////// DSPThreadPool /////////////////////////*/

class DSPThreadPool {
 public:
    static void init();
    static DSPThreadPool& instance(){ return *instance_; }

    DSPThreadPool();
    ~DSPThreadPool();

    using Callback = void (*)(ThreadedPlugin *, int);
    bool push(Callback cb, ThreadedPlugin *plugin, int numSamples);
 private:
    struct Task {
        Callback cb;
        ThreadedPlugin *plugin;
        int numSamples;
    };
    LockfreeFifo<Task, 1024> queue_;
    std::vector<std::thread> threads_;
    std::condition_variable condition_;
    std::atomic_bool running_;
    // simple spin lock
    class SpinLock {
        std::atomic_flag locked_ = ATOMIC_FLAG_INIT;
        char pad[64 - sizeof(locked_)]; // prevent false sharing
    public:
        void lock() {
            while (locked_.test_and_set(std::memory_order_acquire)) { ; }
        }
        void unlock() {
            locked_.clear(std::memory_order_release);
        }
    };
    SpinLock pushLock_;
    SpinLock popLock_;
    // Just leak the instance and don't run the destructor
    // because it might hang/crash the app on exit.
    // We need to figure out why...
    // Also, we don't use a static instance in the instance() method
    // to make *accessing* the instance as fast as possible.
    static DSPThreadPool *instance_;
    static std::mutex initMutex_;
};

DSPThreadPool* DSPThreadPool::instance_ = nullptr;
std::mutex DSPThreadPool::initMutex_{};

// thread-safe initialization
void DSPThreadPool::init(){
    std::lock_guard<std::mutex> lock(initMutex_);
    if (!instance_){
        instance_ = new DSPThreadPool();
    }
}

DSPThreadPool::DSPThreadPool(){
    running_ = true;
    //  number of available hardware threads minus one (= the main audio thread)
    int numThreads = std::max<int>(std::thread::hardware_concurrency() - 1, 1);
    LOG_DEBUG("number of DSP helper threads: " << numThreads);

    for (int i = 0; i < numThreads; ++i){
        std::thread thread([this](){
            setThreadPriority(ThreadPriority::High);
            // We don't really need the mutex (the queue is lock-free),
            // but the interface of std::condition_variable requires it.
            std::mutex dummyMutex;
            std::unique_lock<std::mutex> dummyLock(dummyMutex);
            // the loop:
            while (true) {
                Task task;
                popLock_.lock();
                while (queue_.pop(task)){
                    popLock_.unlock();
                    // call DSP routine
                    task.cb(task.plugin, task.numSamples);
                    popLock_.lock();
                }
                popLock_.unlock();
                if (running_){
                    // wait for more
                    condition_.wait(dummyLock);
                } else {
                    break;
                }
            }
        });
        threads_.push_back(std::move(thread));
    }
}

DSPThreadPool::~DSPThreadPool(){
    // Not actually called... see note above
    // There's also an edge case where a thread might experience a spurious wakeup
    // and the following code runs right between testing "running_" and waiting on
    // the condition variable, but we just ignore this for now.
    running_ = false;
    condition_.notify_all();
    for (auto& thread : threads_){
        if (thread.joinable()){
            thread.join();
        }
    }
}

bool DSPThreadPool::push(Callback cb, ThreadedPlugin *plugin, int numSamples){
    pushLock_.lock();
    bool result = queue_.push({ cb, plugin, numSamples });
    pushLock_.unlock();
    condition_.notify_one();
    return result;
}

/*////////////////////// ThreadedPlugin ///////////////////////*/

ThreadedPlugin::ThreadedPlugin(IPlugin::ptr plugin)
    : plugin_(std::move(plugin)) {
    DSPThreadPool::init();
    event_.signal(); // so that the process routine doesn't wait the very first time
}

ThreadedPlugin::~ThreadedPlugin() {
    // wait for last processing to finish (ideally we shouldn't have to)
    event_.wait();
}

void ThreadedPlugin::lock() {
    mutex_.lock();
    locked_ = true;
}

void ThreadedPlugin::unlock() {
    locked_ = false;
    mutex_.unlock();
}

void ThreadedPlugin::setListener(IPluginListener::ptr listener){
    plugin_->setListener(std::move(listener));
}

void ThreadedPlugin::setupProcessing(double sampleRate, int maxBlockSize, ProcessPrecision precision) {
    // always called in locked state!
    if (locked_){
        plugin_->setupProcessing(sampleRate, maxBlockSize, precision);
        if (maxBlockSize != blockSize_ || precision != precision_){
            blockSize_ = maxBlockSize;
            precision_ = precision;
            updateBuffer();
        }
    } else {
        throw Error("bug in ThreadedPlugin::setNumSpeakers");
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
    assert(buffer == buffer_.end());
}

void ThreadedPlugin::dispatchCommands() {
    // read last queue
    for (auto& command : commands_[!current_]){
        switch(command.type){
        case Command::SetParamValue:
            plugin_->setParameter(command.paramValue.index,
                                  command.paramValue.value, command.paramValue.offset);
            break;
        case Command::SetParamString:
            plugin_->setParameter(command.paramString.index,
                                  command.paramString.string, command.paramString.offset);
            break;
        case Command::Suspend:
            plugin_->suspend();
            break;
        case Command::Resume:
            plugin_->resume();
            break;
        case Command::SetBypass:
            plugin_->setBypass(command.bypass);
            break;
        case Command::SetTempo:
            plugin_->setTempoBPM(command.f);
            break;
        case Command::SetTimeSignature:
            plugin_->setTimeSignature(command.timeSig.num, command.timeSig.denom);
            break;
        case Command::SetTransportPlaying:
            plugin_->setTransportPlaying(command.b);
            break;
        case Command::SetTransportRecording:
            plugin_->setTransportRecording(command.b);
            break;
        case Command::SetTransportAutomationWriting:
            plugin_->setTransportAutomationWriting(command.b);
            break;
        case Command::SetTransportAutomationReading:
            plugin_->setTransportAutomationReading(command.b);
            break;
        case Command::SetTransportCycleActive:
            plugin_->setTransportCycleActive(command.b);
            break;
        case Command::SetTransportCycleStart:
            plugin_->setTransportCycleStart(command.f);
            break;
        case Command::SetTransportCycleEnd:
            plugin_->setTransportCycleEnd(command.f);
            break;
        case Command::SetTransportPosition:
            plugin_->setTransportPosition(command.f);
            break;
        case Command::SendMidi:
            plugin_->sendMidiEvent(command.midi);
            break;
        case Command::SendSysex:
            plugin_->sendSysexEvent(command.sysex);
            free((void *)command.sysex.data);
            break;
        case Command::SetProgram:
            plugin_->setProgram(command.i);
            break;
        case Command::ReadProgramData:
            try {
                plugin_->readProgramData(data_[!current_]);
            } catch (const Error& e){
                // how shall we handle exceptions?
                LOG_ERROR(e.what());
            }
            break;
        case Command::ReadBankData:
            try {
                plugin_->readBankData(data_[!current_]);
            } catch (const Error& e){
                // how shall we handle exceptions?
                LOG_ERROR(e.what());
            }
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
        dispatchCommands();

        ProcessData<T> data;
        data.numSamples = numSamples;
        data.input = (const T **)input_.data();
        data.numInputs = input_.size();
        data.auxInput = (const T **)auxInput_.data();
        data.numAuxInputs = auxInput_.size();
        data.output = (T **)output_.data();
        data.numOutputs = output_.size();
        data.auxOutput = (T **)auxOutput_.data();
        data.numAuxOutputs = auxOutput_.size();

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

    event_.signal();
}

template<typename T>
void ThreadedPlugin::doProcess(ProcessData<T>& data){
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
    // swap queue and notify DSP thread pool
    current_ = !current_;
    auto cb = [](ThreadedPlugin *plugin, int numSamples){
        plugin->threadFunction<T>(numSamples);
    };
    if (!DSPThreadPool::instance().push(cb, this, data.numSamples)){
        LOG_WARNING("couldn't push DSP task!");
        // skip processing and clear outputs
        for (int i = 0; i < data.numOutputs; ++i){
            std::fill(output[i], output[i] + data.numSamples, 0);
        }
        for (int i = 0; i < data.numAuxOutputs; ++i){
            std::fill(auxOutput[i], auxOutput[i] + data.numSamples, 0);
        }
        event_.signal(); // !
    }
}

void ThreadedPlugin::process(ProcessData<float>& data) {
    doProcess(data);
}

void ThreadedPlugin::process(ProcessData<double>& data) {
    doProcess(data);
}

void ThreadedPlugin::suspend() {
    if (locked_){
        // called asynchronously
        plugin_->suspend();
    } else {
        // LATER improve
        std::lock_guard<std::mutex> lock(mutex_);
        plugin_->suspend();
    }
}

void ThreadedPlugin::resume() {
    if (locked_){
        // called asynchronously
        plugin_->resume();
    } else {
        // LATER improve
        std::lock_guard<std::mutex> lock(mutex_);
        plugin_->resume();
    }
}

void ThreadedPlugin::setBypass(Bypass state) {
    Command command(Command::SetBypass);
    command.bypass = state;
    pushCommand(command);
}

void ThreadedPlugin::setNumSpeakers(int in, int out, int auxIn, int auxOut) {
    // always called in locked state!
    if (locked_){
        plugin_->setNumSpeakers(in, out, auxIn, auxOut);
        // floatData and doubleData have the same layout
        input_.resize(in);
        auxInput_.resize(auxIn);
        output_.resize(out);
        auxOutput_.resize(auxOut);
        updateBuffer();
    } else {
        throw Error("bug in ThreadedPlugin::setNumSpeakers");
    }
}

void ThreadedPlugin::setTempoBPM(double tempo) {
    Command command(Command::SetTempo);
    command.f = tempo;
    pushCommand(command);
}

void ThreadedPlugin::setTimeSignature(int numerator, int denominator) {
    Command command(Command::SetTransportPosition);
    command.timeSig.num = numerator;
    command.timeSig.denom = denominator;
    pushCommand(command);
}

void ThreadedPlugin::setTransportPlaying(bool play) {
    Command command(Command::SetTransportPosition);
    command.b = play;
    pushCommand(command);
}

void ThreadedPlugin::setTransportRecording(bool record) {
    Command command(Command::SetTransportRecording);
    command.b = record;
    pushCommand(command);
}

void ThreadedPlugin::setTransportAutomationWriting(bool writing) {
    Command command(Command::SetTransportAutomationWriting);
    command.b = writing;
    pushCommand(command);
}

void ThreadedPlugin::setTransportAutomationReading(bool reading) {
    Command command(Command::SetTransportAutomationReading);
    command.b = reading;
    pushCommand(command);
}

void ThreadedPlugin::setTransportCycleActive(bool active) {
    Command command(Command::SetTransportCycleActive);
    command.b = active;
    pushCommand(command);
}

void ThreadedPlugin::setTransportCycleStart(double beat) {
    Command command(Command::SetTransportCycleStart);
    command.f = beat;
    pushCommand(command);
}

void ThreadedPlugin::setTransportCycleEnd(double beat) {
    Command command(Command::SetTransportCycleEnd);
    command.f = beat;
    pushCommand(command);
}

void ThreadedPlugin::setTransportPosition(double beat) {
    Command command(Command::SetTransportPosition);
    command.f = beat;
    pushCommand(command);
}

void ThreadedPlugin::sendMidiEvent(const MidiEvent& event) {
    Command command(Command::SendMidi);
    auto& midi = command.midi;
    memcpy(midi.data, event.data, sizeof(event.data));
    midi.delta = event.delta;
    midi.detune = event.detune;
    pushCommand(command);
}

void ThreadedPlugin::sendSysexEvent(const SysexEvent& event) {
    // copy data (LATER improve)
    auto data = (char *)malloc(event.size);
    memcpy(data, event.data, event.size);

    Command command(Command::SendSysex);
    auto& sysex = command.sysex;
    sysex.data = data;
    sysex.size = event.size;
    sysex.delta = event.delta;
    pushCommand(command);
}

void ThreadedPlugin::setParameter(int index, float value, int sampleOffset) {
    Command command(Command::SetParamValue);
    auto& param = command.paramValue;
    param.index = index;
    param.value = value;
    param.offset = sampleOffset;
    pushCommand(command);
}

bool ThreadedPlugin::setParameter(int index, const std::string& str, int sampleOffset) {
    // copy string (LATER improve)
    auto buf = (char *)malloc(str.size() + 1);
    memcpy(buf, str.data(), str.size() + 1);

    Command command(Command::SetParamString);
    auto& param = command.paramString;
    param.index = index;
    param.string = buf;
    param.offset = sampleOffset;
    pushCommand(command);

    return true; // what shall we do?
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

void ThreadedPlugin::setProgram(int program) {
    Command command(Command::SetProgram);
    command.i = program;
    pushCommand(command);
}

void ThreadedPlugin::setProgramName(const std::string& name) {
    // LATER improve
    std::lock_guard<std::mutex> lock(mutex_);
    plugin_->setProgramName(name);
}

int ThreadedPlugin::getProgram() const {
    return plugin_->getProgram();
}

std::string ThreadedPlugin::getProgramName() const {
    // LATER improve
    std::lock_guard<std::mutex> lock(mutex_);
    return plugin_->getProgramName();
}

std::string ThreadedPlugin::getProgramNameIndexed(int index) const {
    // LATER improve
    std::lock_guard<std::mutex> lock(mutex_);
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
    if (locked_){
        // called asynchronously
        plugin_->readProgramData(data, size);
    } else {
        data_[current_].assign(data, size);
        pushCommand({Command::ReadProgramData});
    }
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
    if (locked_){
        // called asynchronously
        plugin_->writeProgramData(buffer);
    } else {
        // just lock
        std::lock_guard<std::mutex> lock(mutex_);
        plugin_->writeProgramData(buffer);
    }
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
    if (locked_){
        // called asynchronously
        plugin_->readBankData(data, size);
    } else {
        data_[current_].assign(data, size);
        pushCommand({Command::ReadBankData});
    }
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
    if (locked_){
        // called asynchronously
        plugin_->writeBankData(buffer);
    } else {
        // just lock
        std::lock_guard<std::mutex> lock(mutex_);
        plugin_->writeBankData(buffer);
    }
}

intptr_t ThreadedPlugin::vendorSpecific(int index, intptr_t value, void *p, float opt) {
    if (locked_){
        // called asynchronously
        return plugin_->vendorSpecific(index, value, p, opt);
    } else {
        // LATER improve
        std::lock_guard<std::mutex> lock(mutex_);
        return plugin_->vendorSpecific(index, value, p, opt);
    }
}

} // vst
