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
    auto archs = getPluginCpuArchitectures(path);
    auto hostArch = getHostCpuArchitecture();

    if (std::find(archs.begin(), archs.end(), hostArch) != archs.end()){
        arch_ = hostArch;
    } else {
    #if USE_BRIDGE
        // check if we can bridge any of the given CPU architectures
        for (auto& arch : archs){
            if (IHostApp::get(arch) != nullptr){
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

ProbeFuture PluginFactory::probeAsync(float timeout, bool nonblocking) {
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

PluginFactory::ProbeResultFuture PluginFactory::doProbePlugin(float timeout, bool nonblocking){
    return doProbePlugin(PluginDesc::SubPlugin { "", -1 }, timeout, nonblocking);
}

// probe a plugin in a seperate process and return the info in a file
PluginFactory::ProbeResultFuture PluginFactory::doProbePlugin(
        const PluginDesc::SubPlugin& sub, float timeout, bool nonblocking)
{
    auto desc = std::make_shared<PluginDesc>(shared_from_this());
    desc->name = sub.name; // necessary for error reporting, will be overriden later
    // create temp file path
    std::stringstream ss;
    // desc address should be unique as long as PluginDesc instances are retained.
    ss << getTmpDirectory() << "/vst_" << desc.get();
    std::string tmpPath = ss.str();

    auto app = IHostApp::get(arch_);
    if (!app) {
        // shouldn't happen
        throw Error(Error::SystemError, "couldn't get host app");
    }
    auto process = app->probe(path_, sub.id, tmpPath);
    // NB: std::function doesn't allow move-only types in a lambda capture,
    // so we have to wrap ProcessHandle in a std::shared_ptr...
    return [desc=std::move(desc),
            tmpPath=std::move(tmpPath),
            process=std::make_shared<ProcessHandle>(std::move(process)),
            timeout, nonblocking,
            start=std::chrono::system_clock::now()]
            (ProbeResult& result) {
        result.plugin = desc;
        result.total = 1;
        // wait for process to finish
        int code = -1;
        try {
            if (nonblocking) {
                auto ret = process->tryWait(0);
                if (!ret.first) {
                    if (timeout > 0) {
                        using seconds = std::chrono::duration<double>;
                        auto now = std::chrono::system_clock::now();
                        auto elapsed = std::chrono::duration_cast<seconds>(now - start).count();
                        if (elapsed > timeout) {
                            if (process->terminate()) {
                                LOG_DEBUG("terminated hanging subprocess");
                            }
                            std::stringstream msg;
                            msg << "subprocess timed out after " << timeout << " seconds!";
                            throw Error(Error::SystemError, msg.str());
                        }
                    }
                    return false;
                }
                code = ret.second;
            } else if (timeout > 0) {
                auto ret = process->tryWait(timeout);
                if (!ret.first) {
                    if (process->terminate()) {
                        LOG_DEBUG("terminated hanging subprocess");
                    }
                    std::stringstream msg;
                    msg << "subprocess timed out after " << timeout << " seconds!";
                    throw Error(Error::SystemError, msg.str());
                }
                code = ret.second;
            } else {
                code = process->wait();
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
