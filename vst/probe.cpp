#include "VSTPluginInterface.h"
#include "Utility.h"
#include <stdlib.h>
#include <fstream>

using namespace vst;

#ifdef _WIN32
#define MAIN wmain
#define CHAR wchar_t
#else
#define MAIN main
#define CHAR char
#define shorten(x) x
#endif

// probe a plugin and write info to file
// returns EXIT_SUCCESS on success, EXIT_FAILURE on fail and everything else on error/crash :-)
int MAIN(int argc, const CHAR *argv[]) {
    if (argc >= 3){
		const CHAR *pluginPath = argv[1];
		const CHAR *pluginName = argv[2];
		const CHAR *filePath = argc > 3 ? argv[3] : nullptr;
        try {
            auto factory = vst::IFactory::load(shorten(pluginPath));
            auto plugin = factory->create(shorten(pluginName), true);
            if (filePath) {
                vst::File file(shorten(filePath), File::WRITE);
                if (file.is_open()) {
                    plugin->info().serialize(file);
                } else {
                    LOG_ERROR("probe: couldn't write info file");
                }
            }
            return EXIT_SUCCESS;
        } catch (const Error& e){
            LOG_DEBUG("probe failed: " << e.what());
        }
	}
    return EXIT_FAILURE;
}
