#include "PluginBridge.h"

#include "PluginCommand.h"
#include "Log.h"
#include "CpuArch.h"
#include "MiscUtils.h"

#include <cassert>

namespace vst {

/*///////////////////// Channel /////////////////////*/

template<>
void NRTChannel::checkError(){
    const ShmCommand *reply;
    if (getReply(reply)){
        if (reply->type == Command::Error){
            reply->throwError();
        }
    }
}

/*//////////////////// PluginBridge /////////////////*/

static std::mutex gPluginBridgeMutex;

// use std::weak_ptr, so the bridge is automatically closed if it is not used
static std::unordered_map<CpuArch, std::weak_ptr<PluginBridge>> gPluginBridgeMap;

PluginBridge::ptr PluginBridge::getShared(CpuArch arch){
    PluginBridge::ptr bridge;

    std::lock_guard<std::mutex> lock(gPluginBridgeMutex);

    auto it = gPluginBridgeMap.find(arch);
    if (it != gPluginBridgeMap.end()){
        bridge = it->second.lock();
    }

    // Edge case: the bridge subprocess has crashed, but still lingers,
    // so we create a new one; otherwise, the user would get a misleading
    // error message that the new plugin has crashed.
    if (!bridge || !bridge->alive()){
        // create shared bridge
        LOG_DEBUG("create shared plugin bridge for " << cpuArchToString(arch));
        bridge = std::make_shared<PluginBridge>(arch, true);
        gPluginBridgeMap[arch] = bridge; // insert/assign

        WatchDog::instance().registerProcess(bridge);
    }

    return bridge;
}

PluginBridge::ptr PluginBridge::create(CpuArch arch){
    auto bridge = std::make_shared<PluginBridge>(arch, false);

    WatchDog::instance().registerProcess(bridge);

    return bridge;
}

int getNumDSPThreads();

PluginBridge::PluginBridge(CpuArch arch, bool shared)
    : shared_(shared)
{
    LOG_DEBUG("PluginBridge: created shared memory interface");
    // setup shared memory interface
    // UI channels:
    shm_.addChannel(ShmChannel::Queue, queueSize, "ui_snd");
    shm_.addChannel(ShmChannel::Queue, queueSize, "ui_rcv");
    if (shared){
        // --- shared plugin bridge ---
        // A single NRT channel followed by several RT channels.
        //
        // The bridge can be used from several threads concurrently!
        // This is necessary for hosts with multi-threaded audio processing
        // (like Supernova), some libpd apps - and maybe even Pd itself :-)
        // For the actual channel allocation algorithm, see getRTChannel().
        //
        // NB: getNumDSPThreads() defaults to the number of logical CPUs,
        // unless explicitly overriden by the user (which implies that they
        // really want to use *our* multithreading implemention).
        numThreads_ = getNumDSPThreads();
        LOG_DEBUG("PluginBridge: using " << numThreads_ << " RT threads");
        shm_.addChannel(ShmChannel::Request, nrtRequestSize, "nrt");
        for (int i = 0; i < numThreads_; ++i){
            char buf[16];
            snprintf(buf, sizeof(buf), "rt%d", i+1);
            shm_.addChannel(ShmChannel::Request, rtRequestSize, buf);
        }

        locks_ = std::make_unique<PaddedSpinLock[]>(numThreads_);
    } else {
        // --- sandboxed plugin ---
        // a single rt channel which also doubles as the nrt channel
        shm_.addChannel(ShmChannel::Request, rtRequestSize, "rt");
    }
    shm_.create();

    LOG_DEBUG("PluginBridge: created channels");

#ifdef _WIN32
    // create pipe for logging
    if (!CreatePipe(&hLogRead_, &hLogWrite_, NULL, 0)) {
        throw Error(Error::SystemError,
                    "CreatePipe() failed: " + errorMessage(GetLastError()));
    }
    intptr_t pipeHandle = reinterpret_cast<intptr_t>(hLogWrite_);
#else // Unix
    // create pipe for logging
    int pipefd[2];
    if (pipe(pipefd) != 0){
        throw Error(Error::SystemError,
                    "pipe() failed: " + errorMessage(errno));
    }
    logRead_ = pipefd[0];
    intptr_t pipeHandle = pipefd[1];
#endif
    // spawn host process
    // NB: we already checked in the PluginFactory::PluginFactory if we're able to bridge
    auto hostApp = IHostApp::get(arch);
    assert(hostApp);
    try {
        process_ = hostApp->bridge(shm_.path(), pipeHandle);
    } catch (const Error& e) {
        // close pipe handles
    #ifdef _WIN32
        CloseHandle(hLogRead_);
        CloseHandle(hLogWrite_);
    #else
        close(pipefd[0]);
        close(pipefd[1]);
    #endif
        auto msg = "couldn't create host process '" + hostApp->path() + "': " + e.what();
        throw Error(Error::SystemError, msg);
    }
#ifdef _WIN32
    // We can't simply close our end after CreateProcess() because the child process
    // needs to duplicate the handle; otherwise we would inadvertently close the pipe.
    // We only close our end in the destructor, after reading all remaining messages.
#else
    // close write end *after* creating the subprocess!
    close(pipefd[1]);
#endif

    alive_ = true;
    LOG_DEBUG("PluginBridge: spawned subprocess (child: " << process_.pid()
              << ", parent: " << getCurrentProcessId() << ")");

    pollFunction_ = UIThread::addPollFunction([](void *x){
        static_cast<PluginBridge *>(x)->pollUIThread();
    }, this);
    LOG_DEBUG("PluginBridge: added poll function");
}

PluginBridge::~PluginBridge(){
    LOG_DEBUG("PluginBridge: remove poll function");
    UIThread::removePollFunction(pollFunction_);

    // send quit message
    if (alive()){
        LOG_DEBUG("PluginBridge: send quit message");
        ShmCommand cmd(Command::Quit);

        auto chn = getNRTChannel();
        chn.AddCommand(cmd, empty);
        chn.send();
    }

    // wait for the subprocess to finish.
    // this might even be dangerous if the subprocess
    // somehow got stuck. maybe use some timeout?
    if (process_) {
        LOG_DEBUG("PluginBridge: wait for process");
        try {
            process_.wait();
        } catch (const Error& e) {
            LOG_ERROR("PluginBridge::~PluginBridge: " << e.what());
        }
    }

    // read remaining messages
    readLog(false);

#ifdef _WIN32
    if (hLogRead_) {
        CloseHandle(hLogRead_);
    }
    if (hLogWrite_) {
        CloseHandle(hLogWrite_);
    }
#else
    if (logRead_ >= 0) {
        close(logRead_);
    }
#endif

    LOG_DEBUG("free PluginBridge");
}

void PluginBridge::readLog(bool loud){
#ifdef _WIN32
    if (hLogRead_) {
        for (;;) {
            // try to read header into buffer, but don't remove it from
            // the pipe yet. we also get the number of available bytes.
            // NOTE: PeekNamedPipe() returns immediately!
            LogMessage::Header header;
            DWORD bytesRead, bytesAvailable;
            if (!PeekNamedPipe(hLogRead_, &header, sizeof(header),
                               &bytesRead, &bytesAvailable, NULL)) {
                if (loud) {
                    LOG_ERROR("PeekNamedPipe(): " << errorMessage(GetLastError()));
                }
                CloseHandle(hLogRead_);
                hLogRead_ = NULL;
                return;
            }
            if (bytesRead < sizeof(header)){
                // nothing to read yet
                return;
            }
            // check if message is complete
            int msgsize = header.size + sizeof(header);
            if (bytesAvailable < msgsize) {
                // try again next time
                return;
            }
            // now actually read the whole message
            auto msg = (LogMessage *)alloca(msgsize);
            if (ReadFile(hLogRead_, msg, msgsize, &bytesRead, NULL)){
                if (bytesRead == msgsize){
                    logMessage(msg->header.level, msg->data);
                } else {
                    // shouldn't really happen because we've peeked
                    // the number of available bytes!
                    LOG_ERROR("ReadFile(): size mismatch");
                    CloseHandle(hLogRead_);
                    hLogRead_ = NULL;
                    return;
                }
            } else {
                if (loud) {
                    LOG_ERROR("ReadFile(): " << errorMessage(GetLastError()));
                }
                CloseHandle(hLogRead_);
                hLogRead_ = NULL;
                return;
            }
        }
    }
#else
    if (logRead_ >= 0){
        for (;;){
            auto checkResult = [this, loud](int count){
                if (count > 0){
                    return true;
                }
                if (loud) {
                    if (count == 0){
                        LOG_WARNING("read(): EOF");
                    } else {
                        LOG_ERROR("read(): " << errorMessage(errno));
                    }
                }
                close(logRead_);
                logRead_ = -1;
                return false;
            };

            struct pollfd fds;
            fds.fd = logRead_;
            fds.events = POLLIN;
            fds.revents = 0;

            auto ret = poll(&fds, 1, 0);
            if (ret > 0){
                if (fds.revents & POLLIN){
                    LogMessage::Header header;
                    auto count = read(logRead_, &header, sizeof(header));
                    if (!checkResult(count)){
                        return;
                    }
                    // always atomic (header is smaller than PIPE_BUF).
                    assert(count == sizeof(header));

                    auto msgsize = header.size;
                    auto msg = (char *)alloca(msgsize);
                    // The following calls to read() can block!
                    // This could be dangerous if the subprocess dies after
                    // writing the header but before writing the actual message.
                    // However, in this case all file descriptors to the write end
                    // should have been closed and read() should return 0 (= EOF).
                    //
                    // We use a loop in case the message is larger than PIPE_BUF.
                    int bytes = 0;
                    while (bytes < msgsize) {
                        count = read(logRead_, msg + bytes, msgsize - bytes);
                        if (!checkResult(count)){
                            return;
                        }
                        bytes += count;
                    }
                    logMessage(header.level, msg);
                } else {
                    // pipe closed
                    if (loud) {
                        if (fds.revents & POLLHUP){
                            // there might be remaining data in the pipe, but we don't care.
                            LOG_ERROR("FIFO closed");
                        } else {
                            // shouldn't happen when reading from a pipe
                            LOG_ERROR("FIFO error");
                        }
                    }
                    close(logRead_);
                    logRead_ = -1;
                    break;
                }
            } else if (ret == 0){
                // timeout
                break;
            } else {
                // poll() failed
                LOG_ERROR("poll(): " << errorMessage(errno));
                close(logRead_);
                logRead_ = -1;
                break;
            }
        }
    }
#endif
}


void PluginBridge::checkStatus(){
    // already dead, no need to check
    if (!alive_.load(std::memory_order_acquire)){
        return;
    }
    if (!process_.checkIfRunning()) {
        // make sure that only a single thread will notify clients
        if (alive_.exchange(false)) {
            // notify waiting NRT/RT threads
            for (int i = Channel::NRT; i < shm_.numChannels(); ++i){
                // this should be safe, because channel messages
                // can only be read when they are complete
                // (the channel size is atomic)
                shm_.getChannel(i).postReply();
            }
            LOG_DEBUG("PluginBridge: notify clients");
            // notify all clients
            // NB: clients shall not close the plugin from within
            // the callback function, so this won't deadlock!
            ScopedLock lock(clientMutex_);
            for (auto& it : clients_) {
                it.second->pluginCrashed();
            }
        }
    }
}

void PluginBridge::addUIClient(uint32_t id, IPluginListener* client){
    LOG_DEBUG("PluginBridge: add client " << id);
    ScopedLock lock(clientMutex_);
    clients_.emplace(id, client);
}

void PluginBridge::removeUIClient(uint32_t id){
    LOG_DEBUG("PluginBridge: remove client " << id);
    ScopedLock lock(clientMutex_);
    clients_.erase(id);
}

void PluginBridge::postUIThread(const ShmUICommand& cmd){
    // ScopedLock lock(uiMutex_);
    // sizeof(cmd) is a bit lazy, but we don't care too much about space here
    auto& channel = shm_.getChannel(Channel::UISend);
    if (channel.writeMessage(&cmd, sizeof(cmd))){
        // other side polls regularly
        // channel.post();
    } else {
        // TODO: loop + sleep for 1 second, see PluginServer
        LOG_ERROR("PluginBridge: couldn't post to UI thread");
    }
}

void PluginBridge::pollUIThread(){
    if (!alive()) {
        return;
    }

    auto& channel = shm_.getChannel(Channel::UIReceive);
    char buffer[64]; // larger than ShmCommand!
    size_t size = sizeof(buffer);
    // read all available events
    while (channel.readMessage(buffer, size)){
        auto cmd = (const ShmUICommand *)buffer;
        // find client with matching ID
        ScopedLock lock(clientMutex_);

        auto client = findClient(cmd->id);
        if (client){
            // dispatch events
            switch (cmd->type){
            case Command::ParamAutomated:
                LOG_DEBUG("UI thread: ParameterAutomated");
                client->parameterAutomated(cmd->paramAutomated.index,
                                           cmd->paramAutomated.value);
                break;
            case Command::LatencyChanged:
                LOG_DEBUG("UI thread: LatencyChanged");
                client->latencyChanged(cmd->latency);
                break;
            case Command::UpdateDisplay:
                LOG_DEBUG("UI thread: UpdateDisplay");
                client->updateDisplay();
                break;
            default:
                // ignore other events for now
                break;
            }
        }

        size = sizeof(buffer); // reset size!
    }
}

// must be called with clientMutex_ locked!
IPluginListener* PluginBridge::findClient(uint32_t id){
    auto it = clients_.find(id);
    if (it != clients_.end()){
        return it->second;
    } else {
        LOG_ERROR("PluginBridge::pollUIThread: plugin "
                  << id << " doesn't exist (anymore)");
        return nullptr;
    }
}

RTChannel PluginBridge::getRTChannel(){
    if (locks_){
        // shared plugin bridge, see the comments in PluginBridge::PluginBridge().

        // we map audio threads to successive indices, so that each audio thread
        // is automatically associated with a dedicated thread in the subprocess.
        static std::atomic<uint32_t> counter{0};

        thread_local uint32_t threadIndex = counter.fetch_add(1) % numThreads_;
        uint32_t index = threadIndex;
        if (!locks_[index].try_lock()){
            // if two threads end up on the same spinlock, e.g. because there are
            // more audio threads than in the subprocess, try to find a free spinlock.
            // LOG_DEBUG("PluginBridge: index " << index << " taken");
            do {
                if (++index == numThreads_) {
                    index = 0;
                }
            #if 0
                // pause CPU everytime we cycle through all spinlocks
                if (index == threadIndex) {
                    int spinCount = 1000;
                    while (spinCount--) {
                        pauseCpu();
                    }
                }
            #endif
            } while (!locks_[index].try_lock());
            // LOG_DEBUG("PluginBridge: found free index " << index);
        }
        return RTChannel(shm_.getChannel(Channel::NRT + 1 + index),
                         std::unique_lock<PaddedSpinLock>(locks_[index], std::adopt_lock));
    } else {
        // plugin sandbox: RT channel = NRT channel
        return RTChannel(shm_.getChannel(Channel::NRT));
    }
}

NRTChannel PluginBridge::getNRTChannel(){
    if (locks_){
        return NRTChannel(shm_.getChannel(Channel::NRT),
                          std::unique_lock<Mutex>(nrtMutex_));
    } else {
        // channel 2 is both NRT and RT channel
        return NRTChannel(shm_.getChannel(Channel::NRT));
    }
}

/*/////////////////// WatchDog //////////////////////*/

// poll interval in milliseconds
#define WATCHDOG_POLL_INTERVAL 5

WatchDog& WatchDog::instance(){
    static WatchDog watchDog;
    return watchDog;
}

WatchDog::WatchDog(){
    LOG_DEBUG("start WatchDog");
    running_ = true;
    thread_ = std::thread(&WatchDog::run, this);
}

WatchDog::~WatchDog(){
#ifdef _WIN32
    // You can't synchronize threads in a global/static object
    // destructor in a Windows DLL because of the loader lock.
    // See https://docs.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices
    thread_.detach();
#else
    {
        std::lock_guard<std::mutex> lock(mutex_);
        processes_.clear(); // !
        running_ = false;
        condition_.notify_one();
    }
    thread_.join();
#endif
    LOG_DEBUG("free WatchDog");
}

void WatchDog::registerProcess(PluginBridge::ptr process){
    LOG_DEBUG("WatchDog: register process");
    std::lock_guard<std::mutex> lock(mutex_);
    processes_.push_back(process);
    condition_.notify_one();
}

void WatchDog::run(){
    vst::setThreadPriority(Priority::Low);

    std::unique_lock<std::mutex> lock(mutex_);
    while (running_) {
        LOG_DEBUG("WatchDog: waiting...");
        // wait until a process has been added or we should quit
        condition_.wait(lock, [&]() { return !processes_.empty() || !running_; });
        LOG_DEBUG("WatchDog: woke up");

        // periodically check all running processes
        while (!processes_.empty()){
            for (auto it = processes_.begin(); it != processes_.end();){
                auto process = it->lock();
                if (process){
                    process->readLog();
                    process->checkStatus();
                    ++it;
                } else {
                    // remove stale process
                    it = processes_.erase(it);
                }
            }

            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(WATCHDOG_POLL_INTERVAL));
            lock.lock();
        }
    }
    LOG_DEBUG("WatchDog: thread finished");
}

} // vst
