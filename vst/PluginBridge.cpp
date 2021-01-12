#include "PluginBridge.h"

#include "Utility.h"
#include "PluginCommand.h"

#include <sstream>

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

    if (!bridge){
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

// PluginFactory.cpp
#ifdef _WIN32
const std::wstring& getModuleDirectory();
#else
const std::string& getModuleDirectory();
#endif
std::string getHostApp(CpuArch arch);

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
        // a single nrt channel followed by several rt channels
        shm_.addChannel(ShmChannel::Request, nrtRequestSize, "nrt");
        for (int i = 0; i < maxNumThreads; ++i){
            char buf[16];
            snprintf(buf, sizeof(buf), "rt%d", i+1);
            shm_.addChannel(ShmChannel::Request, rtRequestSize, buf);
        }
        locks_ = std::make_unique<SpinLock[]>(maxNumThreads);
    } else {
        // --- sandboxed plugin ---
        // a single rt channel (which also doubles as the nrt channel)
        shm_.addChannel(ShmChannel::Request, rtRequestSize, "rt");
    }
    shm_.create();

    LOG_DEBUG("PluginBridge: created channels");

    // spawn host process
    std::string hostApp = getHostApp(arch);
#ifdef _WIN32
    // get absolute path to host app
    std::wstring hostPath = getModuleDirectory() + L"\\" + widen(hostApp);
    /// LOG_DEBUG("host path: " << shorten(hostPath));
    // arguments: host.exe bridge <parent_pid> <shm_path>
    // NOTE: on Windows we need to quote the arguments for _spawn to handle
    // spaces in file names.
    std::stringstream cmdLineStream;
    cmdLineStream << hostApp << " bridge "
                  << GetCurrentProcessId() << " \"" << shm_.path() << "\"";
    // LOG_DEBUG(cmdLineStream.str());
    auto cmdLine = widen(cmdLineStream.str());

    ZeroMemory(&pi_, sizeof(pi_));

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
#if BRIDGE_LOG
    // si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
#endif

    if (!CreateProcessW(hostPath.c_str(), &cmdLine[0], NULL, NULL, 0,
                        BRIDGE_LOG ? CREATE_NEW_CONSOLE : DETACHED_PROCESS,
                        NULL, NULL, &si, &pi_)){
        auto err = GetLastError();
        std::stringstream ss;
        ss << "couldn't open host process " << hostApp << " (" << errorMessage(err) << ")";
        throw Error(Error::SystemError, ss.str());
    }
#else // Unix
    // get absolute path to host app
    std::string hostPath = getModuleDirectory() + "/" + hostApp;
    auto parent = std::to_string(getpid());
    // fork
    pid_ = fork();
    if (pid_ == -1) {
        throw Error(Error::SystemError, "fork() failed!");
    } else if (pid_ == 0) {
        // child process: run host app
        // we must not quote arguments to exec!
    #if !BRIDGE_LOG
        // disable stdout and stderr
        auto nullOut = fopen("/dev/null", "w");
        fflush(stdout);
        dup2(fileno(nullOut), STDOUT_FILENO);
        fflush(stderr);
        dup2(fileno(nullOut), STDERR_FILENO);
    #endif
        // arguments: host.exe bridge <parent_pid> <shm_path>
        if (execl(hostPath.c_str(), hostApp.c_str(), "bridge",
                  parent.c_str(), shm_.path().c_str(), 0) < 0){
            // LATER redirect child stderr to parent stdin
            LOG_ERROR("couldn't open host process " << hostApp << " (" << errorMessage(errno) << ")");
        }
        std::exit(EXIT_FAILURE);
    }
#endif
    alive_ = true;
    LOG_DEBUG("PluginBridge: spawned subprocess");

    pollFunction_ = UIThread::addPollFunction([](void *x){
        static_cast<PluginBridge *>(x)->pollUIThread();
    }, this);
    LOG_DEBUG("PluginBridge: added poll function");
}

PluginBridge::~PluginBridge(){
    UIThread::removePollFunction(pollFunction_);

    // send quit message
    if (alive()){
        ShmCommand cmd(Command::Quit);

        auto chn = getNRTChannel();
        chn.AddCommand(cmd, empty);
        chn.send();
    }

    // not really necessary.
    // this might even be dangerous if we accidentally
    // wait on a subprocess that somehow got stuck.
    // maybe use some timeout?
    checkStatus(true);

#ifdef _WIN32
    CloseHandle(pi_.hProcess);
    CloseHandle(pi_.hThread);
#endif

    LOG_DEBUG("free PluginBridge");
}

void PluginBridge::checkStatus(bool wait){
    // already dead, no need to check
    if (!alive_){
        return;
    }
#ifdef _WIN32
    DWORD res = WaitForSingleObject(pi_.hProcess, wait ? INFINITE : 0);
    if (res == WAIT_TIMEOUT){
        return; // still running
    } else if (res == WAIT_OBJECT_0){
        DWORD code = 0;
        if (GetExitCodeProcess(pi_.hProcess, &code)){
            if (code == EXIT_SUCCESS){
                LOG_DEBUG("host process exited successfully");
            } else if (code == EXIT_FAILURE){
                LOG_ERROR("host process exited with failure");
            } else {
                LOG_ERROR("host process crashed!");
            }
        } else {
            LOG_ERROR("couldn't retrieve exit code for host process!");
        }
    } else {
        LOG_ERROR("WaitForSingleObject() failed");
    }
#else
    int code = -1;
    int status = 0;
    if (waitpid(pid_, &status, wait ? 0 : WNOHANG) == 0){
        return; // still running
    }
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
        if (code == EXIT_SUCCESS){
            LOG_DEBUG("host process exited successfully");
        } else if (code == EXIT_FAILURE){
            LOG_ERROR("host process exited with failure");
        } else {
            LOG_ERROR("host process crashed!");
        }
    } else {
        LOG_ERROR("couldn't get exit status");
    }
#endif

    bool wasAlive = alive_.exchange(false);

    // notify waiting RT threads
    for (int i = 2; i < shm_.numChannels(); ++i){
        // this should be safe, because channel messages
        // can only be read when they are complete
        // (the channel size is atomic)
        shm_.getChannel(i).postReply();
    }

    if (wasAlive){
        // notify all clients
        for (auto& it : clients_){
            auto client = it.second.lock();
            if (client){
                client->pluginCrashed();
            }
        }
    }
}

void PluginBridge::addUIClient(uint32_t id, std::shared_ptr<IPluginListener> client){
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
    auto& channel = shm_.getChannel(0);
    if (channel.writeMessage((const char *)&cmd, sizeof(cmd))){
        // other side polls regularly
        // channel.post();
    } else {
        LOG_ERROR("PluginBridge: couldn't post to UI thread");
    }
}

void PluginBridge::pollUIThread(){
    auto& channel = shm_.getChannel(1);

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
            default:
                // ignore other events for now
                break;
            }
        }

        size = sizeof(buffer); // reset size!
    }
}

IPluginListener::ptr PluginBridge::findClient(uint32_t id){
    auto it = clients_.find(id);
    if (it != clients_.end()){
        auto client = it->second.lock();
        if (client){
            return client;
        } else {
            LOG_ERROR("PluginBridge::pollUIThread: plugin "
                        << id << " is stale");
        }
    } else {
        LOG_ERROR("PluginBridge::pollUIThread: plugin "
                  << id << " doesn't exist (anymore)");
    }
    return nullptr;
}

RTChannel PluginBridge::getRTChannel(){
    if (locks_){
        static std::atomic<uint32_t> counter{0}; // can safely overflow

        uint32_t index;
        for (;;){
            // we take the current index and try to lock the
            // corresponding spinlock. if the spinlock is already
            // taken (another DSP thread trying to use the plugin bridge
            // concurrently), we atomically increment the index and try again.
            // if there's only a single DSP thread, we will only ever
            // lock the first spinlock and the plugin server will only
            // use a single thread as well.

            // modulo is optimized to bitwise AND
            // if maxNumThreads is power of 2!
            index = counter.load(std::memory_order_acquire)
                    % maxNumThreads;
            if (locks_[index].try_lock()){
                break;
            }
            ++counter; // atomic increment
        }
        return RTChannel(shm_.getChannel(index + 3),
                         std::unique_lock<SpinLock>(locks_[index], std::adopt_lock));
    } else {
        // channel 2 is both NRT and RT channel
        return RTChannel(shm_.getChannel(2));
    }
}

NRTChannel PluginBridge::getNRTChannel(){
    if (locks_){
        return NRTChannel(shm_.getChannel(2),
                          std::unique_lock<Mutex>(nrtMutex_));
    } else {
        // channel 2 is both NRT and RT channel
        return NRTChannel(shm_.getChannel(2));
    }
}

/*/////////////////// WatchDog //////////////////////*/

WatchDog& WatchDog::instance(){
    static WatchDog watchDog;
    return watchDog;
}

WatchDog::WatchDog(){
    LOG_DEBUG("create WatchDog");
    running_ = true;
    thread_ = std::thread([this](){
        vst::setThreadPriority(Priority::Low);

        std::unique_lock<std::mutex> lock(mutex_);
        for (;;){
            // periodically check all running processes
            while (!processes_.empty()){
                for (auto it = processes_.begin(); it != processes_.end();){
                    auto process = it->lock();
                    if (process){
                        process->checkStatus();
                        ++it;
                    } else {
                        // remove stale process
                        it = processes_.erase(it);
                    }
                }

                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                lock.lock();
            }

            // wait for a new process to be added
            //
            // NOTE: we have to put both the check and wait at the end!
            // Since the thread is created concurrently with the first process,
            // the process might be registered *before* the thread starts,
            // causing the latter to wait on the condition variable instead of
            // entering the poll loop.
            // Also, 'running_' might be set to false while we're sleeping in the
            // poll loop, which means that we would wait on the condition forever.
            if (running_){
                LOG_DEBUG("WatchDog: waiting...");
                condition_.wait(lock);
                LOG_DEBUG("WatchDog: new process registered");
            } else {
                break;
            }
        }
        LOG_DEBUG("WatchDog: thread finished");
    });
#if !WATCHDOG_JOIN
    thread_.detach();
#endif
}

WatchDog::~WatchDog(){
#if WATCHDOG_JOIN
    {
        std::lock_guard<std::mutex> lock(mutex_);
        processes_.clear();
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

} // vst
