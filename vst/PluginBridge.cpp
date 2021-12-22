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
        // a single NRT channel followed by several RT channels.
        //
        // the bridge can be used from several threads concurrently!
        // this is necessary for hosts with multi-threaded audio processing,
        // like Supernova, some libpd apps - and maybe even Pd itself :-)
        // for the actual algorithm, see getRTChannel().
        static int hwThreads = []() {
            return std::thread::hardware_concurrency();
        }();
        auto numThreads = prevPowerOfTwo(hwThreads);
        LOG_DEBUG("PluginBridge: using " << numThreads << " RT threads");
        threadMask_ = numThreads - 1;
        shm_.addChannel(ShmChannel::Request, nrtRequestSize, "nrt");
        for (int i = 0; i < numThreads; ++i){
            char buf[16];
            snprintf(buf, sizeof(buf), "rt%d", i+1);
            shm_.addChannel(ShmChannel::Request, rtRequestSize, buf);
        }

        locks_ = std::make_unique<PaddedSpinLock[]>(numThreads);
    } else {
        // --- sandboxed plugin ---
        // a single rt channel which also doubles as the nrt channel
        shm_.addChannel(ShmChannel::Request, rtRequestSize, "rt");
    }
    shm_.create();

    LOG_DEBUG("PluginBridge: created channels");

    // spawn host process
    std::string hostApp = getHostApp(arch);

#ifdef _WIN32
    // create pipe for logging
    if (!CreatePipe(&hLogRead_, &hLogWrite_, NULL, 0)) {
        throw Error(Error::SystemError,
                    "CreatePipe() failed: " + errorMessage(GetLastError()));
    }
    // the write handle gets passed to the child process. we can't simply close
    // our end after CreateProcess() because the child process needs to duplicate
    // the handle; otherwise we would inadvertently close the pipe. we only close
    // our end in the destructor, after reading all remaining messages.

    // NOTE: Win32 handles can be safely truncated to 32-bit!
    auto writeLog = (uint32_t)reinterpret_cast<uintptr_t>(hLogWrite_);

    // get process Id
    auto parent = GetCurrentProcessId();
    // get absolute path to host app
    std::wstring hostPath = getModuleDirectory() + L"\\" + widen(hostApp);
    /// LOG_DEBUG("host path: " << shorten(hostPath));
    // arguments: host.exe bridge <parent_pid> <shm_path>
    // NOTE: we need to quote string arguments (in case they contain spaces)
    std::stringstream cmdLineStream;
    cmdLineStream << hostApp << " bridge " << parent
                  << " \"" << shm_.path() << "\" " << writeLog;
    // LOG_DEBUG(cmdLineStream.str());
    auto cmdLine = widen(cmdLineStream.str());

    ZeroMemory(&pi_, sizeof(pi_));

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(hostPath.c_str(), &cmdLine[0], NULL, NULL, 0,
                        BRIDGE_LOG ? CREATE_NEW_CONSOLE : DETACHED_PROCESS,
                        NULL, NULL, &si, &pi_)){
        auto err = GetLastError();
        std::stringstream ss;
        ss << "couldn't open host process " << hostApp << " (" << errorMessage(err) << ")";
        throw Error(Error::SystemError, ss.str());
    }
    auto pid = pi_.dwProcessId;
#else // Unix
    // create pipe
    int pipefd[2];
    if (pipe(pipefd) != 0){
        throw Error(Error::SystemError,
                    "pipe() failed: " + errorMessage(errno));
    }
    auto writeEnd = std::to_string(pipefd[1]);
    // get parent pid
    auto parent = getpid();
    auto parentStr = std::to_string(parent);
    // get absolute path to host app
    std::string hostPath = getModuleDirectory() + "/" + hostApp;
    // fork
#if !BRIDGE_LOG
    // flush before fork() to avoid duplicate printouts!
    fflush(stdout);
    fflush(stderr);
#endif
    auto pid = fork();
    if (pid == -1) {
        throw Error(Error::SystemError, "fork() failed!");
    } else if (pid == 0) {
        // child process
        close(pipefd[0]); // close read end

    #if !BRIDGE_LOG
        // disable stdout and stderr
        auto devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
    #endif

        // host.exe bridge <parent_pid> <shm_path> <logchannel>
        // NOTE: we must not quote arguments to exec!
    #if USE_WINE
        if (arch == CpuArch::pe_i386 || arch == CpuArch::pe_amd64){
            // run in Wine
            auto winecmd = getWineCommand();
            // LOG_DEBUG("host path: " << hostPath);
            // arguments: wine host.exe bridge <parent_pid> <shm_path>
            // NOTE: we must *not* quote arguments to exec()
            // execlp() beause we want to use PATH!
            if (execlp(winecmd, winecmd, hostPath.c_str(), "bridge",
                       parentStr.c_str(), shm_.path().c_str(),
                       writeEnd.c_str(), nullptr) != 0)
            {
                LOG_ERROR("couldn't run '" << winecmd
                          << "' (" << errorMessage(errno) << ")");
            }
            std::exit(EXIT_FAILURE);
        }
    #endif
        // LOG_DEBUG("host path: " << hostPath);
        // arguments: host.exe bridge <parent_pid> <shm_path>
        if (execl(hostPath.c_str(), hostApp.c_str(), "bridge",
                  parentStr.c_str(), shm_.path().c_str(),
                  writeEnd.c_str(), nullptr) != 0)
        {
            LOG_ERROR("couldn't open host process "
                      << hostApp << " (" << errorMessage(errno) << ")");
        }
        std::exit(EXIT_FAILURE);
    }
    // parent process
    pid_ = pid;
    logRead_ = pipefd[0];
    close(pipefd[1]); // close write end!
#endif
    alive_ = true;
    LOG_DEBUG("PluginBridge: spawned subprocess (child: " << pid
              << ", parent: " << parent << ")");

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

    // wait for the subprocess to finish.
    // this might even be dangerous if the subprocess
    // somehow got stuck. maybe use some timeout?
    checkStatus(true);
    // read remaining messages
    readLog();

#ifdef _WIN32
    CloseHandle(pi_.hProcess);
    CloseHandle(pi_.hThread);
    CloseHandle(hLogRead_);
    CloseHandle(hLogWrite_);
#endif

    LOG_DEBUG("free PluginBridge");
}

void PluginBridge::readLog(){
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
                LOG_ERROR("PeekNamedPipe(): " << errorMessage(GetLastError()));
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
                    // shouldn't really happen, because we've peeked
                    // the number of available bytes!
                    LOG_ERROR("ReadFile(): size mismatch");
                    CloseHandle(hLogRead_);
                    hLogRead_ = NULL;
                    return;
                }
            } else {
                LOG_ERROR("ReadFile(): " << errorMessage(GetLastError()));
                CloseHandle(hLogRead_);
                hLogRead_ = NULL;
                return;
            }
        }
    }
#else
    if (logRead_ >= 0){
        for (;;){
            auto checkResult = [this](int count){
                if (count > 0){
                    return true;
                } else if (count == 0){
                    LOG_WARNING("read(): EOF");
                } else {
                    LOG_ERROR("read(): " << errorMessage(errno));
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
                    if (fds.revents & POLLHUP){
                        // there might be remaining data in the pipe, but we don't care.
                        LOG_ERROR("FIFO closed");
                    } else {
                        // shouldn't happen when reading from a pipe
                        LOG_ERROR("FIFO error");
                    }
                    close(logRead_);
                    logRead_ = -1;
                    break;
                }
            } else if (ret == 0){
                // timeout
                break;
            } else {
                LOG_ERROR("poll(): " << errorMessage(errno));
                break;
            }
        }
    }
#endif
}


void PluginBridge::checkStatus(bool wait){
    // already dead, no need to check
    if (!alive_.load()){
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
                LOG_DEBUG("Watchdog: subprocess exited successfully");
            } else if (code == EXIT_FAILURE){
                // LATER get the actual Error from the child process.
                LOG_WARNING("Watchdog: subprocess exited with failure");
            } else {
                LOG_WARNING("Watchdog: subprocess crashed!");
            }
        } else {
            LOG_ERROR("Watchdog: couldn't retrieve exit code for subprocess!");
        }
    } else {
        LOG_ERROR("Watchdog: WaitForSingleObject() failed: "
                  + errorMessage(GetLastError()));
    }
#else
    int status = 0;
    auto ret = waitpid(pid_, &status, wait ? 0 : WNOHANG);
    if (ret == 0){
        return; // still running
    } else if (ret > 0){
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == EXIT_SUCCESS){
                LOG_DEBUG("Watchdog: subprocess exited successfully");
            } else if (code == EXIT_FAILURE){
                // LATER get the actual Error from the child process.
                LOG_WARNING("Watchdog: subprocess exited with failure");
            } else {
                LOG_WARNING("Watchdog: subprocess crashed!");
            }
        } else if (WIFSIGNALED(status)){
            auto sig = WTERMSIG(status);
            LOG_WARNING("Watchdog: subprocess was terminated with signal "
                        << sig << " (" << strsignal(sig) << ")");
        } else if (WIFSTOPPED(status)){
            auto sig = WSTOPSIG(status);
            LOG_WARNING("Watchdog: subprocess was stopped with signal "
                        << sig << " (" << strsignal(sig) << ")");
        } else if (WIFCONTINUED(status)){
            // FIXME what should be do here?
            LOG_VERBOSE("Watchdog: subprocess continued");
        } else {
            LOG_ERROR("Watchdog: unknown status (" << status << ")");
        }
    } else {
        LOG_ERROR("Watchdog: waitpid() failed: " << errorMessage(errno));
    }
#endif

    bool wasAlive = alive_.exchange(false);

    // notify waiting NRT/RT threads
    for (int i = Channel::NRT; i < shm_.numChannels(); ++i){
        // this should be safe, because channel messages
        // can only be read when they are complete
        // (the channel size is atomic)
        shm_.getChannel(i).postReply();
    }

    if (wasAlive){
        LOG_DEBUG("PluginBridge: notify clients");
        // notify all clients
        ScopedLock lock(clientMutex_);
        for (auto& it : clients_){
            auto client = it.second.lock();
            if (client){
                client->pluginCrashed();
            } else {
                LOG_DEBUG("PluginBridge: stale client");
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
    auto& channel = shm_.getChannel(Channel::UISend);
    if (channel.writeMessage(&cmd, sizeof(cmd))){
        // other side polls regularly
        // channel.post();
    } else {
        LOG_ERROR("PluginBridge: couldn't post to UI thread");
    }
}

void PluginBridge::pollUIThread(){
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
        // shared plugin bridge, see the comments in PluginBridge::PluginBridge().
        static std::atomic<uint32_t> counter{0}; // can safely overflow

        uint32_t index, i = 0;
        uint32_t mask = threadMask_;
        for (;;){
            // we take the current index and try to lock the corresponding spinlock.
            // if the spinlock is already taken (another DSP thread is trying to use the
            // plugin bridge oncurrently), we atomically increment the index and try again.
            // if there's only a single DSP thread, we will only ever lock the first spinlock
            // and the plugin server will only use a single thread as well.
            index = counter.load(std::memory_order_acquire) & mask;
            if (locks_[index].try_lock()){
                break;
            }
            counter.fetch_add(1);
        #if 0
            // pause CPU everytime we cycle through all spinlocks
            const int spin_count = 1000;
            if ((++i & mask) == 0) {
                for (int j = 0; j < spin_count; ++j) {
                    pauseCpu();
                }
            }
        #endif
        }
        return RTChannel(shm_.getChannel(Channel::NRT + 1),
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
    LOG_DEBUG("create WatchDog");
    running_ = true;
    thread_ = std::thread(&WatchDog::run, this);
#if !WATCHDOG_JOIN
    thread_.detach();
#endif
}

WatchDog::~WatchDog(){
#if WATCHDOG_JOIN
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
                    process->checkStatus(false);
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
