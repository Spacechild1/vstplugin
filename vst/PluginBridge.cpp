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

std::mutex gPluginBridgeMutex;

// use std::weak_ptr, so the bridge is automatically closed if it is not used
std::unordered_map<CpuArch, std::weak_ptr<PluginBridge>> gPluginBridgeMap;

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
        gPluginBridgeMap.emplace(arch, bridge);

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
        auto pid = std::to_string(getpid());
        if (execl(hostPath.c_str(), hostApp.c_str(), "bridge",
                  pid.c_str(), shm_.path().c_str()) < 0){
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
#ifdef _WIN32
    CloseHandle(pi_.hProcess);
    CloseHandle(pi_.hThread);
#endif
}

void PluginBridge::checkStatus(){
    // already dead, no need to check
    if (!alive_){
        return;
    }
#ifdef _WIN32
    DWORD res = WaitForSingleObject(pi_.hProcess, 0);
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
    if (waitpid(pid_, &status, WNOHANG) == 0){
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

    alive_ = false;

    // notify waiting RT threads
    for (int i = 2; i < shm_.numChannels(); ++i){
        shm_.getChannel(i).postReply();
    }
}

void PluginBridge::addUIClient(uint32_t id, std::shared_ptr<IPluginListener> client){
    LockGuard lock(clientMutex_);
    clients_.emplace(id, client);
}

void PluginBridge::removeUIClient(uint32_t id){
    LockGuard lock(clientMutex_);
    clients_.erase(id);
}

bool PluginBridge::postUIThread(const ShmUICommand& cmd){
    // LockGuard lock(uiMutex_);
    // sizeof(cmd) is a bit lazy, but we don't care too much about space here
    auto& channel = shm_.getChannel(0);
    auto success = channel.writeMessage((const char *)&cmd, sizeof(cmd));
    if (success){
        channel.post();
    }
    return success;
}

void PluginBridge::pollUIThread(){
    auto& channel = shm_.getChannel(1);

    ShmUICommand cmd;
    size_t size = sizeof(cmd);
    // read all available events
    while (channel.readMessage((char *)&cmd, size)){
        // find client with matching ID
        LockGuard lock(clientMutex_);
        auto it = clients_.find(cmd.id);
        if (it != clients_.end()){
            auto listener = it->second.lock();
            if (listener){
                // dispatch events
                switch (cmd.type){
                case Command::ParamAutomated:
                    listener->parameterAutomated(cmd.paramAutomated.index,
                                                 cmd.paramAutomated.value);
                    break;
                default:
                    // ignore other events for now
                    break;
                }
            } else {
                LOG_ERROR("PluginBridge::pollUIThread: plugin "
                            << cmd.id << " is stale");
            }
        } else {
            LOG_ERROR("PluginBridge::pollUIThread: plugin "
                      << cmd.id << " doesn't exist (anymore)");
        }
        size = sizeof(cmd); // reset size!
    }
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
        return RTChannel(shm_.getChannel(index + 3), locks_[index]);
    } else {
        return RTChannel(shm_.getChannel(2));
    }
}

NRTChannel PluginBridge::getNRTChannel(){
    if (locks_){
        return NRTChannel(shm_.getChannel(2), nrtMutex_);
    } else {
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
            // NOTE: we have to put both the check and wait at the end,
            // because the thread is created concurrently with the first item
            // and 'running_' might be set to false during this_thread::sleep_for().
            if (running_){
                condition_.wait(lock);
            } else {
                break;
            }
        }
    });
#if !WATCHDOG_JOIN
    thread_.detach();
#endif
    LOG_DEBUG("create WatchDog done");
}

WatchDog::~WatchDog(){
    processes_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool running_ = false;
        condition_.notify_one();
    }
#if WATCHDOG_JOIN
    thread_.join();
#endif
}

void WatchDog::registerProcess(PluginBridge::ptr process){
    LOG_DEBUG("WatchDog: register process");
    std::lock_guard<std::mutex> lock(mutex_);
    processes_.push_back(process);
    // wake up if process list has been empty!
    if (processes_.size() == 1){
        condition_.notify_one();
    }
}

} // vst
