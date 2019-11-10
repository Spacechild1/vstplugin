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

// probe a plugin and write info to file
// returns EXIT_SUCCESS on success, EXIT_FAILURE on fail and everything else on error/crash :-)
#ifdef _WIN32
int wmain(int argc, const wchar_t *argv[]){
#else
int main(int argc, const char *argv[]) {
#endif
    int status = EXIT_FAILURE;
    if (argc >= 2){
        std::string pluginPath = shorten(argv[1]);
        std::string pluginName = argc > 2 ? shorten(argv[2]) : "";
        std::string filePath = argc > 3 ? shorten(argv[3]) : "";
        LOG_DEBUG("probing " << pluginPath << " " << pluginName);
        try {
            auto factory = vst::IFactory::load(pluginPath);
            auto plugin = factory->create(pluginName, true);
            if (!filePath.empty()) {
                vst::File file(filePath, File::WRITE);
                if (file.is_open()) {
                    plugin->info().serialize(file);
                } else {
                    LOG_ERROR("ERROR: couldn't write info file");
                }
            }
            status = EXIT_SUCCESS;
            LOG_VERBOSE("probe succeeded");
        } catch (const Error& e){
            writeErrorMsg(e.code(), e.what(), filePath);
            LOG_ERROR("probe failed: " << e.what());
        } catch (const std::exception& e) {
            writeErrorMsg(Error::UnknownError, e.what(), filePath);
            LOG_ERROR("probe failed: " << e.what());
        }
    }
    return status;
}
