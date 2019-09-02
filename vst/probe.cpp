#include "Interface.h"
#include "Utility.h"
#include <stdlib.h>

using namespace vst;

#ifndef _WIN32
 #define shorten(x) x
#endif

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
        LOG_DEBUG("probe.exe: " << pluginPath << ", " << pluginName);
        try {
            auto factory = vst::IFactory::load(pluginPath);
            auto plugin = factory->create(pluginName, true);
            if (!filePath.empty()) {
                vst::File file(filePath, File::WRITE);
                if (file.is_open()) {
                    plugin->info().serialize(file);
                } else {
                    LOG_ERROR("probe: couldn't write info file");
                }
            }
            status = EXIT_SUCCESS;
            LOG_VERBOSE("probe succeeded");
        } catch (const std::exception& e){
            // catch any exception!
            LOG_ERROR("probe failed: " << e.what());
        }
    }
    return status;
}
