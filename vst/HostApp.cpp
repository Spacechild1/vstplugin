#include "HostApp.h"
#include "MiscUtils.h"
#include "CpuArch.h"
#include "FileUtils.h"
#include "Log.h"

#include <mutex>
#include <cassert>

namespace vst {

std::string getHostAppName(CpuArch arch){
    if (arch == getHostCpuArchitecture()){
    #ifdef _WIN32
        return "host.exe";
    #else
        return "host";
    #endif
    } else {
        std::string host = std::string("host_") + cpuArchToString(arch);
    #if defined(_WIN32)
        host += ".exe";
    #endif
        return host;
    }
}

//----------------------- ProcessHandle -----------------------------//

#ifdef _WIN32

int ProcessHandle::pid() const {
    return pi_.dwProcessId;
}

int ProcessHandle::wait() {
    auto result = tryWait(-1);
    assert(result.first);
    return result.second;
}

std::pair<bool, int> ProcessHandle::tryWait(double timeout) {
    // don't remove the DWORD cast!
    const DWORD timeoutms = (timeout >= 0) ? static_cast<DWORD>(timeout * 1000.f) : INFINITE;
    auto res = WaitForSingleObject(pi_.hProcess, timeoutms);
    if (res == WAIT_TIMEOUT){
        return { false, -1 };
    } else if (res == WAIT_OBJECT_0){
        DWORD code;
        if (!GetExitCodeProcess(pi_.hProcess, &code)) {
            throw Error(Error::SystemError, "couldn't retrieve exit code for subprocess!");
        }
        return { true, code };
    } else {
        throw Error(Error::SystemError, "WaitForSingleObject() failed: " + errorMessage(GetLastError()));
    }
}

bool ProcessHandle::checkIfRunning() {
    DWORD res = WaitForSingleObject(pi_.hProcess, 0);
    if (res == WAIT_TIMEOUT){
        return true; // still running
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
    return false;
}

bool ProcessHandle::terminate() {
    if (TerminateProcess(pi_.hProcess, EXIT_FAILURE)) {
        return true;
    } else {
        LOG_ERROR("couldn't terminate subprocess: "
                  << errorMessage(GetLastError()));
        return false;
    }
}

#else

int ProcessHandle::pid() const {
    return pid_;
}

int ProcessHandle::wait() {
    int status = 0;
    if (waitpid(pid, &status, 0) != pid_){
        std::stringstream msg;
        msg << "waitpid() failed " << errorMessage(errno) << ")";
        throw Error(Error::SystemError, msg.str());
    }
    return parseStatus(status);
}

std::pair<bool, int> ProcessHandle::tryWait(double timeout) {
    int status = 0;
    if (timeout == 0) {
        auto ret = waitpid(pid, &status, WNOHANG);
        if (ret == 0) {
            return { false, -1 };
        } else if (ret < 0) {
            std::stringstream msg;
            msg << "waitpid() failed " << errorMessage(errno) << ")";
            throw Error(Error::SystemError, msg.str());
        }
    } else {
        // HACK: poll in a loop with exponential back-off
        int sleepmicros = 1000;
        const int maxsleep = 100000; // 100 ms
        auto start = std::chrono::system_clock::now();
        for (;;) {
            auto ret = waitpid(pid, &status, WNOHANG);
            if (ret == pid_) {
                break;
            } else if (ret == 0) {
                auto now = std::chrono::system_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
                if (elapsed >= timeout) {
                    return { false, -1 };
                } else {
                    usleep(sleepmicros);
                    sleepmicros *= 2;
                    if (sleepmicros > maxsleep) {
                        sleepmicros = maxsleep;
                    }
                }
            } else if (ret < 0) {
                std::stringstream msg;
                msg << "waitpid() failed " << errorMessage(errno) << ")";
                throw Error(Error::SystemError, msg.str());
            }
        }
    }
    return { true, parseStatus(status) };
}

bool ProcessHandle::checkIfRunning() {
    int status = 0;
    auto ret = waitpid(pid_, &status, WNOHANG);
    if (ret == 0){
        return true; // still running
    } else if (ret == pid_) {
        // subprocess changed state
        try {
            int code = parseStatus(status);
            if (code == EXIT_SUCCESS){
                LOG_DEBUG("Watchdog: subprocess exited successfully");
            } else if (code == EXIT_FAILURE){
                // LATER get the actual Error from the child process.
                LOG_WARNING("Watchdog: subprocess exited with failure");
            } else {
                LOG_WARNING("Watchdog: subprocess crashed!");
            }
        } catch (const Error& e) {
            LOG_WARNING("WatchDog: " << e.what());
        }
    } else {
        LOG_ERROR("Watchdog: waitpid() failed: " << errorMessage(errno));
    }
    return false;
}

bool ProcessHandle::terminate() {
    if (kill(pid, SIGTERM) == 0){
        return true;
    } else {
        LOG_ERROR("couldn't terminate subprocess: " << errorMessage(errno));
        return false;
    }
}

int ProcessHandle::parseStatus(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)){
        auto sig = WTERMSIG(status);
        std::stringstream msg;
        msg << "subprocess was terminated with signal "
           << sig << " (" << strsignal(sig) << ")";
        throw Error(Error::SystemError, msg.str());
    } else if (WIFSTOPPED(status)){
        auto sig = WSTOPSIG(status);
        std::stringstream msg;
        msg << "subprocess was stopped with signal "
           << sig << " (" << strsignal(sig) << ")";
        throw Error(Error::SystemError, msg.str());
    } else if (WIFCONTINUED(status)){
        // FIXME what should be do here?
        throw Error(Error::SystemError, "subprocess continued");
    } else {
        std::stringstream msg;
        msg << "unknown exit status (" << status << ")";
        throw Error(Error::SystemError, msg.str());
    }
}

#endif // _WIN32

//--------------------------- HostApp ------------------------------//

// should host.exe inherit file handles and print to stdout/stderr?
#define PROBE_LOG 0

// redirect stdout and stderr from child process to parent.
// use this if you want to see debug output from the actual VST plugins.
// NOTE: this doesn't affect log functions like LOG_ERROR because
// they go to a dedicated log pipe.
#ifndef BRIDGE_LOG
#define BRIDGE_LOG 0
#endif

class HostApp : public IHostApp {
public:
    HostApp(CpuArch arch, const std::string& path)
        : arch_(arch), path_(path) {}

    CpuArch arch() const override {
        return arch_;
    }

    const std::string& path() const override {
        return path_;
    }

    ProcessHandle probe(const std::string& path, int id,
                        const std::string& tmpPath) const override;

    ProcessHandle bridge(const std::string& shmPath, intptr_t logPipe) const override;

    virtual bool test() const {
        return doTest(path_);
    }
protected:
    CpuArch arch_;
    std::string path_;

    bool doTest(const std::string& cmd, const std::string& args = "") const;

#ifdef _WIN32
    ProcessHandle createProcess(const std::string& cmdline, bool log) const;
#else
    template<bool log, typename T...>
    static ProcessHandle createProcess(const char *cmd, T&&... args) const;
#endif
};

bool HostApp::doTest(const std::string& cmd, const std::string& args) const {
    std::stringstream ss;
    ss << args << " test " << getVersionString();
    try {
        int exitCode = runCommand(cmd, ss.str());
        if (exitCode == EXIT_SUCCESS){
            return true; // success
        } else if (exitCode == EXIT_FAILURE) {
            LOG_ERROR("host app '" << path_ << "' failed (version mismatch)");
        } else {
            LOG_ERROR("host app '" << path_ << "' failed with exit code " << exitCode);
        }
    } catch (const Error& e) {
        LOG_ERROR("failed to execute host app '" << path_ << "': " << e.what());
    }
    return false;
}

#ifdef _WIN32

ProcessHandle HostApp::probe(const std::string &pluginPath, int id,
                             const std::string &tmpPath) const {
    // turn id into hex string
    char idString[12];
    if (id >= 0){
        snprintf(idString, sizeof(idString), "0x%X", id);
    } else {
        sprintf(idString, "_");
    }
    /// LOG_DEBUG("host path: " << path_);
    /// LOG_DEBUG("temp path: " << tmpPath);
    // arguments: host.exe probe <plugin_path> <plugin_id> <file_path>
    // NOTE: we need to quote string arguments (in case they contain spaces)
    std::stringstream cmdline;
    cmdline << fileName(path_) << " probe "
            << "\"" << pluginPath << "\" " << idString
            << " \"" << tmpPath + "\"";

    return createProcess(cmdline.str(), PROBE_LOG);
}

ProcessHandle HostApp::bridge(const std::string &shmPath, intptr_t logPipe) const {
    /// LOG_DEBUG("host path: " << shorten(hostPath));
    // arguments: host.exe bridge <parent_pid> <shm_path>
    // NOTE: we need to quote string arguments (in case they contain spaces)
    // NOTE: Win32 handles can be safely cast to DWORD!
    std::stringstream cmdline;
    cmdline << fileName(path_) << " bridge " << getCurrentProcessId()
            << " \"" << shmPath << "\" " << (DWORD)logPipe;

    return createProcess(cmdline.str(), BRIDGE_LOG);
}

ProcessHandle HostApp::createProcess(const std::string &cmdline, bool log) const {
    // LOG_DEBUG(path_ << " " << cmdline);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    if (!CreateProcessW(widen(path_).c_str(), widen(cmdline).data(), NULL, NULL, 0,
                        log ? CREATE_NEW_CONSOLE : DETACHED_PROCESS,
                        NULL, NULL, &si, &pi)){
        auto err = GetLastError();
        std::stringstream ss;
        ss << "couldn't open host process " << path_ << " (" << errorMessage(err) << ")";
        throw Error(Error::SystemError, ss.str());
    }

    return ProcessHandle(pi);
}

#else

template<bool log, typename T... args>
ProcessHandle HostApp::createProcess(const char *cmd, T&&... args) const {
    if (!log) {
        // flush before fork() to avoid duplicate printouts!
        fflush(stdout);
        fflush(stderr);
    }
    auto pid = fork();
    if (pid == -1) {
        throw Error(Error::SystemError, "fork() failed!");
    } else if (pid == 0) {
        // child process
        if (!log) {
            // disable stdout and stderr
            auto nullOut = fopen("/dev/null", "w");
            dup2(fileno(nullOut), STDOUT_FILENO);
            dup2(fileno(nullOut), STDERR_FILENO);
        }
        // NOTE: we must not quote arguments to exec!
        // NOTE: use PATH for "arch", "wine", etc.
        if (execlp(cmd, args..., nullptr) < 0) {
            LOG_ERROR("execl() failed: " << errorMessage(errno));
        }
        std::exit(EXIT_FAILURE);
    }
    return ProcessHandle(pid);
}

ProcessHandle HostApp::probe(const std::string &pluginPath, int id,
                             const std::string &tmpPath) {
    // turn id into hex string
    char idString[12];
    if (id >= 0){
        snprintf(idString, sizeof(idString), "0x%X", id);
    } else {
        sprintf(idString, "_");
    }
    // arguments: host probe <plugin_path> <plugin_id> <file_path>
    return createProcess<PROBE_LOG>(path_.c_str(), fileName(path_).c_str(), "probe",
                                    pluginPath.c_str(), idString, tmpPath.c_str());
}

ProcessHandle HostApp::bridge(const std::string &shmPath, intptr_t logPipe) {
    auto parent = std::to_string(getpid());
    auto pipe = std::to_string(static_cast<int>(logPipe));
    // arguments: host bridge <parent_pid> <shm_path>
    return createProcess<BRIDGE_LOG>(path_.c_str(), fileName(path_).c_str(), "bridge",
                                     shmPath.c_str(), parent.c_str(), pipe.c_str());
}

#endif

#ifdef __APPLE__
const char * archOption(CpuArch arch) {
    if (arch == CpuArch::aarch64) {
        return "-arm64";
    } else if (arch == CpuArch::amd64) {
        return "-x86_64";
    } else if (arch == CpuArch::i386) {
        return "-i386";
    } else {
        std::stringstream ss;
        ss << "unsupported CPU architecture " << cpuArchToString();
        throw Error(Error::ModuleError, ss.str());
    }
}

class UniversalHostApp : public HostApp {
    using HostApp::HostApp;

    ProcessHandle probe(const std::string& path, int id,
                        const std::string& tmpPath) override {
        // turn id into hex string
        char idString[12];
        if (id >= 0){
            snprintf(idString, sizeof(idString), "0x%X", id);
        } else {
            sprintf(idString, "_");
        }
        // arguments: arch -<arch> <host_path> probe <plugin_path> <plugin_id> <file_path>
        return createProcess<PROBE_LOG>("arch", "arch", archOption(arch_), path_.c_str(), "probe",
                                        pluginPath.c_str(), idString, tmpPath.c_str());
    }

    ProcessHandle bridge(const std::string& shmPath, intptr_t logPipe) override {
        auto parent = std::to_string(getpid());
        auto pipe = std::to_string(static_cast<int>(logPipe));
        // arguments: arch -<arch> <host_path> bridge <parent_pid> <shm_path>
        return createProcess<BRIDGE_LOG>("arch", "arch", archOption(arch_), path_.c_str(), "bridge",
                                         shmPath.c_str(), parent.c_str(), pipe.c_str());
    }

    bool test() const override {
        std::stringstream ss;
        ss << archOption(arch_) << " \"" << path_ << "\"";
        return doTest("arch", ss.str());
    }
};
#endif

#if USE_BRIDGE && !defined(_WIN32)
class WineHostApp : public HostApp {
    using HostApp::HostApp;

    ProcessHandle probe(const std::string& pluginPath, int id,
                        const std::string& tmpPath) override {
        auto wine = getWineCommand();
        // turn id into hex string
        char idString[12];
        if (id >= 0){
            snprintf(idString, sizeof(idString), "0x%X", id);
        } else {
            sprintf(idString, "_");
        }
        // arguments: wine <host_path> probe <plugin_path> <plugin_id> <file_path>
        return createProcess<PROBE_LOG>(wine, wine, path_.c_str(), "probe",
                                        pluginPath.c_str(), idString, tmpPath.c_str());
    }

    ProcessHandle bridge(const std::string& shmPath, intptr_t logPipe) override {
        auto wine = getWineCommand();
        auto parent = std::to_string(getpid());
        auto pipe = std::to_string(static_cast<int>(logPipe));
        // arguments: wine <host_path> bridge <parent_pid> <shm_path>
        return createProcess<BRIDGE_LOG>(wine, wine, path_.c_str(), "bridge",
                                         shmPath.c_str(), parent.c_str(), pipe.c_str());
    }

    bool test() const override {
        std::stringstream ss;
        ss << "\"" << path_ << "\"";
        return doTest(getWineCommand(), ss.str());
    }
};
#endif

std::mutex gHostAppMutex;

std::unordered_map<CpuArch, std::unique_ptr<IHostApp>> gHostAppDict;

// Generally, we can bridge between any kinds of CPU architectures,
// as long as the they are supported by the platform in question.
//
// We use the following naming scheme for the plugin bridge app:
// host_<cpu_arch>[extension]
// Examples: "host_i386", "host_amd64.exe", etc.
//
// We can selectively enable/disable CPU architectures simply by
// including resp. omitting the corresponding app.
// Note that we always ship a version of the *same* CPU architecture
// called "host" resp. "host.exe" to support plugin sandboxing.
//
// Bridging between i386 and amd64 is typically employed on Windows,
// but also possible on Linux and macOS (before 10.15).
// On the upcoming ARM MacBooks, we can also bridge between amd64 and aarch64.
// NOTE: We ship 64-bit Intel builds on Linux without "host_i386" and
// ask people to compile it themselves if they need it.
//
// On macOS and Linux we can also use the plugin bridge to run Windows plugins
// via Wine. The apps are called "host_pe_i386.exe" and "host_pe_amd64.exe".

// we only have to protect against concurrent insertion to avoid race conditions
// during the lookup process. The actual IHostApp pointer will never be invalidated.
IHostApp* IHostApp::get(CpuArch arch) {
    std::lock_guard<std::mutex> lock(gHostAppMutex);
    auto it = gHostAppDict.find(arch);
    if (it != gHostAppDict.end()){
        return it->second.get();
    }

#if USE_WINE
    bool isWine = false;
    if (arch == CpuArch::pe_i386 || arch == CpuArch::pe_amd64){
        // check if the 'wine' command can be found and works.
        if (!haveWine()){
            gHostAppDict[arch] = nullptr;
            return nullptr;
        }
        isWine = true;
    }
#endif // USE_WINE

#ifdef _WIN32
    auto path = getModuleDirectory() + "\\" + getHostAppName(arch);
#else
    auto path = getModuleDirectory() + "/" + getHostAppName(arch);
#endif

    // check if host app exists and works
    if (pathExists(path)){
    #if USE_WINE
        std::unique_ptr<HostApp> app = isWine ? std::make_unique<WineHostApp>(path) :
                                                std::make_unique<HostApp>(path);
    #else
        auto app = std::make_unique<HostApp>(arch, path);
    #endif
        if (app->test()) {
            LOG_DEBUG("host app '" << path << "' is working");
            auto result = gHostAppDict.emplace(arch, std::move(app));
            return result.first->second.get();
        } else {
            gHostAppDict[arch] = nullptr;
            return nullptr;
        }
    } else {
    #ifdef __APPLE__
        // check if "host" is a universal binary that contains the desired architecture
        path = getModuleDirectory() + "/host";
        for (auto& a : getFileCpuArchitectures(path)) {
            if (a == arch) {
                auto app = std::make_unique<UniversalHostApp>(path);
                if (app->test()) {
                    LOG_DEBUG("host app '" << path << "' ("
                              << cpuArchToString(arch) << ") is working");
                    auto result = gHostAppDict.emplace(arch, app);
                    return result.first.get();
                } else {
                    gHostAppDict[arch] = nullptr;
                    return nullptr;
                }
            }
        }
#endif
        LOG_VERBOSE("no appropriate host app for CPU architecture " << cpuArchToString(arch));
        gHostAppDict[arch] = nullptr;
        return nullptr;
    }
}

} // vst
