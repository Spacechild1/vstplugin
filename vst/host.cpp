#include "Interface.h"
#include "PluginDesc.h"
#include "Log.h"
#include "FileUtils.h"
#include "MiscUtils.h"
#if USE_BRIDGE
#include "PluginServer.h"
#endif

#include <stdlib.h>

#if !defined(_WIN32) || defined(__WINE__)
# define USE_ALARM 1
# include <signal.h>
#else
# define USE_ALARM 0
#endif

using namespace vst;

#ifndef USE_WMAIN
# ifdef _WIN32
#  define USE_WMAIN 1
# else
#  define USE_WMAIN 0
# endif
#endif

#if !USE_WMAIN
# define shorten(x) x
#endif

void writeErrorMsg(Error::ErrorCode code, const char* msg, const std::string& path){
    if (!path.empty()){
        // this part should be async signal safe
        vst::File file(path, File::WRITE);
        if (file.is_open()) {
            file << static_cast<int>(code) << "\n";
            file << msg << "\n";
        } else {
            LOG_ERROR("ERROR: couldn't write error message");
        }
    }
}

#if USE_ALARM
static std::string gFilePath;
static int gTimeout;
#endif

// probe a plugin and write info to file
// returns EXIT_SUCCESS on success, EXIT_FAILURE on fail and everything else on error/crash :-)
int probe(const std::string& pluginPath, int pluginIndex,
          const std::string& filePath, float timeout)
{
    setProcessPriority(Priority::Low);
    setThreadPriority(Priority::Low);

#if USE_ALARM
    if (timeout > 0){
        gFilePath = filePath;
        gTimeout = timeout + 0.5; // round up

        signal(SIGALRM, [](int){
        #if 1
            // not really async safe
            LOG_DEBUG("probe timed out");
        #endif
            // this should be more or less safe
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "subprocess timed out after %d seconds", gTimeout);
            writeErrorMsg(Error::SystemError, buf, gFilePath);
            std::exit(EXIT_FAILURE);
        });

        alarm(gTimeout); // round up
    }
#endif

    LOG_DEBUG("probing " << pluginPath << " " << pluginIndex);
    try {
        auto factory = vst::IFactory::load(pluginPath, true);
        auto desc = factory->probePlugin(pluginIndex);
        if (!filePath.empty()) {
            vst::File file(filePath, File::WRITE);
            if (file.is_open()) {
                desc->serialize(file);
            } else {
                LOG_ERROR("ERROR: couldn't write info file " << filePath);
            }
        }
        LOG_VERBOSE("probe succeeded");
        return EXIT_SUCCESS;
    } catch (const Error& e){
        writeErrorMsg(e.code(), e.what(), filePath);
        LOG_ERROR("probe failed: " << e.what());
    } catch (const std::exception& e) {
        writeErrorMsg(Error::UnknownError, e.what(), filePath);
        LOG_ERROR("probe failed: " << e.what());
    } catch (...) {
        writeErrorMsg(Error::UnknownError, "unknown exception", filePath);
        LOG_ERROR("probe failed: unknown exception");
    }
    return EXIT_FAILURE;
}

#if USE_BRIDGE
// host one or more VST plugins
int bridge(int pid, const std::string& path){
    LOG_DEBUG("bridge begin: " << pid << " " << path);
    setProcessPriority(Priority::High);
    // main thread is UI thread
    setThreadPriority(Priority::Low);
    try {
        // create and run server
        auto server = std::make_unique<PluginServer>(pid, path);

        server->run();

        LOG_DEBUG("bridge end");

        return EXIT_SUCCESS;
    } catch (const Error& e){
        // LATER redirect stderr to parent stdin to get the error message
        LOG_ERROR(e.what());
        return EXIT_FAILURE;
    }
}
#endif

#if USE_WMAIN
int wmain(int argc, const wchar_t *argv[]){
#else
int main(int argc, const char *argv[]) {
#endif
    if (argc >= 2){
        std::string verb = shorten(argv[1]);
        argc -= 2;
        argv += 2;
        if (verb == "probe" && argc > 0){
            // args: <plugin_path> [<id>] [<file_path>]
            std::string path = shorten(argv[0]);
            int index;
            try {
                index = std::stol(argv[1], 0, 0);
            } catch (...){
                index = -1; // non-numeric argument
            }
            std::string file = argc > 2 ? shorten(argv[2]) : "";
            float timeout;
            try {
                timeout = argc > 3 ? std::stof(argv[3]) : 0.f;
            } catch (...){
                timeout = 0.f;
            }
            return probe(path, index, file, timeout);
        }
    #if USE_BRIDGE
        else if (verb == "bridge" && argc >= 2){
            // args: <pid> <shared_mem_path>
            int pid = std::stod(argv[0]);
            std::string shmPath = shorten(argv[1]);

            return bridge(pid, shmPath);
        }
    #endif
        else if (verb == "test"){
            return EXIT_SUCCESS;
        }
    }
    LOG_ERROR("usage:");
    LOG_ERROR("  probe <plugin_path> [<id>] [<file_path>]");
#if USE_BRIDGE
    LOG_ERROR("  bridge <pid> <shared_mem_path>");
#endif
    LOG_ERROR("  test");
    return EXIT_FAILURE;
}
