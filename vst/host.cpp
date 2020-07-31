#include "Interface.h"
#include "Utility.h"
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

int bridge(const std::string& path){
    LOG_VERBOSE("not implemented yet");
    return EXIT_FAILURE;
}

int probe(const std::string& pluginPath, const std::string& pluginName,
          const std::string& filePath)
{
    LOG_DEBUG("probing " << pluginPath << " " << pluginName);
    try {
        auto factory = vst::IFactory::load(pluginPath);
        auto plugin = factory->create(pluginName, true);
        if (!filePath.empty()) {
            vst::File file(filePath, File::WRITE);
            if (file.is_open()) {
                plugin->info().serialize(file);
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
            std::string path = shorten(argv[0]);
            std::string name = argc > 1 ? shorten(argv[1]) : "";
            std::string file = argc > 2 ? shorten(argv[2]) : "";
            return probe(path, name, file);
        } else if (verb == "bridge" && argc > 0){
            return bridge(shorten(argv[0]));
        }
    }
    LOG_ERROR("usage:");
    LOG_ERROR("  probe <plugin_path> [<plugin_name|shell_id>] [<file_path>]");
    LOG_ERROR("  bridge <shared_mem_path>");
    return EXIT_FAILURE;
}
