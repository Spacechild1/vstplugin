#include "PluginFactory.h"

#include "Log.h"
#include "FileUtils.h"
#include "MiscUtils.h"
#if USE_VST2
 #include "VST2Plugin.h"
#endif
#if USE_VST3
 #include "VST3Plugin.h"
#endif

// for probing
#ifdef _WIN32
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#else
# include <unistd.h>
# include <stdlib.h>
# include <stdio.h>
# include <signal.h>
# include <sys/wait.h>
#endif

#include <thread>
#include <algorithm>
#include <sstream>

namespace vst {

/*///////////////////// IFactory ////////////////////////*/

IFactory::ptr IFactory::load(const std::string& path, bool probe){
    // LOG_DEBUG("IFactory: loading " << path);
    auto ext = fileExtension(path);
    if (ext == ".vst3"){
    #if USE_VST3
        if (!pathExists(path)){
            throw Error(Error::ModuleError, "No such file");
        }
        return std::make_shared<VST3Factory>(path, probe);
    #else
        throw Error(Error::ModuleError, "VST3 plug-ins not supported");
    #endif
    } else {
    #if USE_VST2
        std::string realPath = path;
        if (ext.empty()){
            // no extension: assume VST2 plugin
        #ifdef _WIN32
            realPath += ".dll";
        #elif defined(__APPLE__)
            realPath += ".vst";
        #else // Linux/BSD/etc.
            realPath += ".so";
        #endif
        }
        if (!pathExists(realPath)){
            throw Error(Error::ModuleError, "No such file");
        }
        return std::make_shared<VST2Factory>(realPath, probe);
    #else
        throw Error(Error::ModuleError, "VST2 plug-ins not supported");
    #endif
    }
}

/*/////////////////////////// PluginFactory ////////////////////////*/

PluginFactory::PluginFactory(const std::string &path)
    : path_(path)
{
    auto archs = getCpuArchitectures(path);
    auto hostArch = getHostCpuArchitecture();

    if (std::find(archs.begin(), archs.end(), hostArch) != archs.end()){
        arch_ = hostArch;
    } else {
    #if USE_BRIDGE
        // check if we can bridge any of the given CPU architectures
        for (auto& arch : archs){
            if (canBridgeCpuArch(arch)){
                arch_ = arch;
                // LOG_DEBUG("created bridged plugin factory " << path);
                return;
            }
        }
        // fail
        if (archs.size() > 1){
            throw Error(Error::ModuleError, "Can't bridge CPU architectures");
        } else {
            throw Error(Error::ModuleError, "Can't bridge CPU architecture "
                        + std::string(cpuArchToString(archs.front())));
        }
    #else // USE_BRIDGE
        if (archs.size() > 1){
            throw Error(Error::ModuleError, "Unsupported CPU architectures");
        } else {
            throw Error(Error::ModuleError, "Unsupported CPU architecture "
                        + std::string(cpuArchToString(archs.front())));
        }
    #endif // USE_BRIDGE
    }
}

IFactory::ProbeFuture PluginFactory::probeAsync(float timeout, bool nonblocking) {
    plugins_.clear();
    pluginMap_.clear();
    return [this, self = shared_from_this(), timeout,
            f = doProbePlugin(timeout, nonblocking)]
            (ProbeCallback callback) {
        // call future
        ProbeResult result;
        if (f(result)){
            if (result.plugin->subPlugins.empty()){
                // factory only contains a single plugin
                if (result.valid()) {
                    plugins_ = { result.plugin };
                }
                if (callback){
                    callback(result);
                }
            } else {
                // factory contains several subplugins
                plugins_ = doProbePlugins(result.plugin->subPlugins,
                                          timeout, callback);
            }
            for (auto& desc : plugins_) {
                pluginMap_[desc->name] = desc;
            }
            return true;
        } else {
            return false;
        }
    };
}

void PluginFactory::addPlugin(PluginDesc::ptr desc){
    if (!pluginMap_.count(desc->name)){
        plugins_.push_back(desc);
        pluginMap_[desc->name] = desc;
    }
}

PluginDesc::const_ptr PluginFactory::getPlugin(int index) const {
    if (index >= 0 && index < (int)plugins_.size()){
        return plugins_[index];
    } else {
        return nullptr;
    }
}

PluginDesc::const_ptr PluginFactory::findPlugin(const std::string& name) const {
    auto it = pluginMap_.find(name);
    if (it != pluginMap_.end()){
        return it->second;
    } else {
        return nullptr;
    }
}

int PluginFactory::numPlugins() const {
    return plugins_.size();
}

// should host.exe inherit file handles and print to stdout/stderr?
#define PROBE_LOG 0

PluginFactory::ProbeResultFuture PluginFactory::doProbePlugin(float timeout, bool nonblocking){
    return doProbePlugin(PluginDesc::SubPlugin { "", -1 }, timeout, nonblocking);
}

// probe a plugin in a seperate process and return the info in a file
PluginFactory::ProbeResultFuture PluginFactory::doProbePlugin(
        const PluginDesc::SubPlugin& sub, float timeout, bool nonblocking)
{
    auto desc = std::make_shared<PluginDesc>(shared_from_this());
    desc->name = sub.name; // necessary for error reporting, will be overriden later
    // turn id into string
    char idString[12];
    if (sub.id >= 0){
        snprintf(idString, sizeof(idString), "0x%X", sub.id);
    } else {
        sprintf(idString, "_");
    }
    // create temp file path
    std::stringstream ss;
    // desc address should be unique as long as PluginDesc instances are retained.
    ss << getTmpDirectory() << "/vst_" << desc.get();
    std::string tmpPath = ss.str();
    // LOG_DEBUG("temp path: " << tmpPath);
    std::string hostApp = getHostApp(arch_);
#ifdef _WIN32
    // get absolute path to host app
    std::wstring hostPath = getModuleDirectory() + L"\\" + widen(hostApp);
    /// LOG_DEBUG("host path: " << shorten(hostPath));
    // arguments: host.exe probe <plugin_path> <plugin_id> <file_path>
    // on Windows we need to quote the arguments for _spawn to handle spaces in file names.
    std::stringstream cmdLineStream;
    cmdLineStream << hostApp << " probe "
            << "\"" << path() << "\" " << idString
            << " \"" << tmpPath + "\"";
    // LOG_DEBUG(cmdLineStream.str());
    auto cmdLine = widen(cmdLineStream.str());
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(hostPath.c_str(), &cmdLine[0], NULL, NULL,
                        PROBE_LOG, DETACHED_PROCESS, NULL, NULL, &si, &pi)){
        auto err = GetLastError();
        std::stringstream ss;
        ss << "couldn't open host process " << hostApp << " (" << errorMessage(err) << ")";
        throw Error(Error::SystemError, ss.str());
    }
    auto wait = [pi, timeout, nonblocking](DWORD& code){
        // don't remove the DWORD cast!
        const DWORD timeoutms = (timeout > 0) ? static_cast<DWORD>(timeout * 1000.f) : INFINITE;
        auto res = WaitForSingleObject(pi.hProcess, nonblocking ? 0 : timeoutms);
        if (res == WAIT_TIMEOUT){
            if (nonblocking){
                return false;
            } else {
                if (TerminateProcess(pi.hProcess, EXIT_FAILURE)){
                    LOG_DEBUG("terminated hanging subprocess");
                } else {
                    LOG_ERROR("couldn't terminate hanging subprocess: "
                              << errorMessage(GetLastError()));
                }
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                std::stringstream msg;
                msg << "subprocess timed out after " << timeout << " seconds!";
                throw Error(Error::SystemError, msg.str());
            }
        } else if (res == WAIT_OBJECT_0){
            if (!GetExitCodeProcess(pi.hProcess, &code)){
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                throw Error(Error::SystemError, "couldn't retrieve exit code for subprocess!");
            }
        } else {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            throw Error(Error::SystemError, "WaitForSingleObject() failed: " + errorMessage(GetLastError()));
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    };
#else // Unix
    // get absolute path to host app
    std::string hostPath = getModuleDirectory() + "/" + hostApp;
    // timeout
    auto timeoutstr = std::to_string(timeout);
    // fork
#if !PROBE_LOG
    // flush before fork() to avoid duplicate printouts!
    fflush(stdout);
    fflush(stderr);
#endif
    pid_t pid = fork();
    if (pid == -1) {
        throw Error(Error::SystemError, "fork() failed!");
    } else if (pid == 0) {
        // child process: start new process with plugin path and temp file path as arguments.
        // we must not quote arguments to exec!
    #if !PROBE_LOG
        // disable stdout and stderr
        auto nullOut = fopen("/dev/null", "w");
        dup2(fileno(nullOut), STDOUT_FILENO);
        dup2(fileno(nullOut), STDERR_FILENO);
    #endif
        // arguments: host probe <plugin_path> <plugin_id> <file_path> [timeout]
    #if USE_WINE
        if (arch_ == CpuArch::pe_i386 || arch_ == CpuArch::pe_amd64){
            const char *winecmd = getWineCommand();
            // use PATH!
            if (execlp(winecmd, winecmd, hostPath.c_str(), "probe", path().c_str(),
                       idString, tmpPath.c_str(), timeoutstr.c_str(), nullptr) < 0) {
                // LATER redirect child stderr to parent stdin
                LOG_ERROR("couldn't run 'wine' (" << errorMessage(errno) << ")");
            }
        } else
    #endif
        if (execl(hostPath.c_str(), hostApp.c_str(), "probe", path().c_str(),
                  idString, tmpPath.c_str(), timeoutstr.c_str(), nullptr) < 0) {
            // write error to temp file
            int err = errno;
            File file(tmpPath, File::WRITE);
            if (file.is_open()){
                file << static_cast<int>(Error::SystemError) << "\n";
                file << "couldn't run subprocess " << hostApp
                     << ": " << errorMessage(err) << "\n";
            }
        }
        std::exit(EXIT_FAILURE);
    }
    // parent process: wait for child
    auto wait = [pid, nonblocking](int& code){
        int status = 0;
        if (waitpid(pid, &status, nonblocking ? WNOHANG : 0) == 0){
            return false;
        }
        if (WIFEXITED(status)) {
            code = WEXITSTATUS(status);
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
        return true;
    };
#endif
    return [desc=std::move(desc),
            tmpPath=std::move(tmpPath),
            wait=std::move(wait),
        #ifdef _WIN32
            pi,
        #else
            pid,
        #endif
            timeout,
            start = std::chrono::system_clock::now()]
            (ProbeResult& result) {
    #ifdef _WIN32
        DWORD code;
    #else
        int code;
    #endif
        result.plugin = desc;
        result.total = 1;
        // wait for process to finish
        // (returns false when nonblocking and still running)
        try {
            if (!wait(code)){
                if (timeout > 0) {
                    using seconds = std::chrono::duration<double>;
                    auto now = std::chrono::system_clock::now();
                    auto elapsed = std::chrono::duration_cast<seconds>(now - start).count();
                    if (elapsed > timeout){
                    #ifdef _WIN32
                        if (TerminateProcess(pi.hProcess, EXIT_FAILURE)){
                            LOG_DEBUG("terminated hanging subprocess");
                        } else {
                            LOG_ERROR("couldn't terminate hanging subprocess: "
                                      << errorMessage(GetLastError()));
                        }
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                    #else
                        if (kill(pid, SIGTERM) == 0){
                            LOG_DEBUG("terminated hanging subprocess");
                        } else {
                            LOG_ERROR("couldn't terminate hanging subprocess: "
                                      << errorMessage(errno));
                        }
                    #endif
                        std::stringstream msg;
                        msg << "subprocess timed out after " << timeout << " seconds!";
                        throw Error(Error::SystemError, msg.str());
                    }
                }
                return false;
            }
        } catch (const Error& e){
            result.error = e;
            return true;
        }
        /// LOG_DEBUG("return code: " << ret);
        TmpFile file(tmpPath); // removes the file on destruction
        if (code == EXIT_SUCCESS) {
            // get info from temp file
            if (file.is_open()) {
                try {
                    desc->deserialize(file);
                } catch (const Error& e) {
                    result.error = e;
                }
            } else {
            #if USE_WINE
                // On Wine, the child process (wine) might exit with 0
                // even though the grandchild (= host) has crashed.
                // The missing temp file is the only indicator we have...
                if (desc->arch() == CpuArch::pe_amd64 || desc->arch() == CpuArch::pe_i386){
                #if 1
                    result.error = Error(Error::SystemError,
                                         "couldn't read temp file (plugin crashed?)");
                #else
                    result.error = Error(Error::Crash);
                #endif
                } else
            #endif
                {
                    result.error = Error(Error::SystemError, "couldn't read temp file!");
                }
            }
        } else if (code == EXIT_FAILURE) {
            // get error from temp file
            if (file.is_open()) {
                int err;
                std::string msg;
                file >> err;
                if (file){
                    std::getline(file, msg); // skip newline
                    std::getline(file, msg); // read message
                } else {
                    // happens in certain cases, e.g. the plugin destructor
                    // terminates the probe process with exit code 1.
                    err = (int)Error::UnknownError;
                    msg = "(uncaught exception)";
                }
                LOG_DEBUG("code: " << err << ", msg: " << msg);
                result.error = Error((Error::ErrorCode)err, msg);
            } else {
                result.error = Error(Error::UnknownError, "(uncaught exception)");
            }
        } else {
            // ignore temp file
            result.error = Error(Error::Crash);
        }
        return true;
    };
}

std::vector<PluginDesc::ptr> PluginFactory::doProbePlugins(
        const PluginDesc::SubPluginList& pluginList,
        float timeout, ProbeCallback callback)
{
    std::vector<PluginDesc::ptr> results;
    int numPlugins = pluginList.size();
#ifdef PLUGIN_LIMIT
    numPlugins = std::min<int>(numPlugins, PLUGIN_LIMIT);
#endif
    // LOG_DEBUG("numPlugins: " << numPlugins);
    auto plugin = pluginList.begin();
    int count = 0;
    int maxNumFutures = std::min<int>(PROBE_FUTURES, numPlugins);
    std::vector<ProbeResultFuture> futures;
    while (count < numPlugins) {
        // push futures
        while (futures.size() < maxNumFutures && plugin != pluginList.end()){
            try {
                futures.push_back(doProbePlugin(*plugin++, timeout, true));
            } catch (const Error& e){
                // return error future
                futures.push_back([=](ProbeResult& result){
                    result.error = e;
                    return true;
                });
            }
        }
        // collect results
        for (auto it = futures.begin(); it != futures.end();) {
            ProbeResult result;
            // call future (non-blocking)
            if ((*it)(result)){
                result.index = count++;
                result.total = numPlugins;
                if (result.valid()) {
                    results.push_back(result.plugin);
                }
                if (callback){
                    callback(result);
                }
                // remove future
                it = futures.erase(it);
            } else {
                it++;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(PROBE_SLEEP_MS));
    }
    return results;
}

} // vst
