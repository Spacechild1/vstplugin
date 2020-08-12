#include "Interface.h"
#include "Utility.h"
#if USE_BRIDGE
#include "PluginServer.h"
#endif

#include <stdlib.h>

using namespace vst;

#ifndef _WIN32
 #define shorten(x) x
#endif

namespace {

void writeErrorMsg(Error::ErrorCode code, const char* msg, const std::string& path){
    if (!path.empty()){
        vst::File file(path, File::WRITE);
        if (file.is_open()) {
            file << static_cast<int>(code) << "\n";
            file << msg << "\n";
        } else {
            LOG_ERROR("ERROR: couldn't write error message");
        }
    }
}

} // namespace

int probe(const std::string& pluginPath, int pluginIndex,
          const std::string& filePath)
{
    setProcessPriority(Priority::Low);
    setThreadPriority(Priority::Low);

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
    }
    return EXIT_FAILURE;
}

// probe a plugin and write info to file
// returns EXIT_SUCCESS on success, EXIT_FAILURE on fail and everything else on error/crash :-)
#ifdef _WIN32
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
            return probe(path, index, file);
        }
    #if USE_BRIDGE
        else if (verb == "bridge" && argc >= 2){
            // args: <pid> <shared_mem_path> [<plugin_path> <plugin_name>]
            int pid = std::stod(argv[0]);
            std::string shmPath = shorten(argv[1]);

            try {
                std::unique_ptr<PluginServer> server_;
                if (argc > 3){
                    // sandboxed plugin
                    std::string pluginPath = shorten(argv[2]);
                    std::string pluginName = shorten(argv[3]);
                    server_ = std::make_unique<PluginServer>(pid, shmPath,
                                                             pluginPath, pluginName);
                } else {
                    // shared plugin bridge
                    server_ = std::make_unique<PluginServer>(pid, shmPath);
                }
                // run the server
                server_->run();

                return EXIT_SUCCESS;
            } catch (const Error& e){
                // LATER redirect stderr to parent stdin to get the error message
                LOG_ERROR(e.what());
                return EXIT_FAILURE;
            }
        }
    #endif
    }
    LOG_ERROR("usage:");
    LOG_ERROR("  probe <plugin_path> [<id>] [<file_path>]");
#if USE_BRIDGE
    LOG_ERROR("  bridge <pid> <shared_mem_path> [<plugin_path> <plugin_name>]");
#endif
    return EXIT_FAILURE;
}
