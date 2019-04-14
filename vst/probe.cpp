#include "VSTPluginInterface.h"
#include "Utility.h"
#include <stdlib.h>

#ifdef _WIN32
#define MAIN wmain
#define CHAR wchar_t
namespace vst {
	std::string shorten(const std::wstring&);
}
#define shorten(x) vst::shorten(x)
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
        /// LOG_DEBUG("probe: pluginPath '" << pluginPath << "', pluginName, '" << pluginName);
		auto factory = vst::IVSTFactory::load(shorten(pluginPath));
		if (factory) {
            auto plugin = factory->create(shorten(pluginName), true);
			if (plugin) {
				if (filePath) {
					vst::VSTPluginDesc desc(*factory, *plugin);
					// there's no way to open a fstream with a wide character path...
					// (the C++17 standard allows filesystem::path but this isn't widely available yet)
					// for now let's assume temp paths are always ASCII. LATER fix this!
					std::ofstream file(shorten(filePath), std::ios::binary);
					if (file.is_open()) {
						desc.serialize(file);
						/// LOG_DEBUG("info written");
					}
				}
				/// LOG_DEBUG("probe success");
				return EXIT_SUCCESS;
			}
			/// LOG_DEBUG("couldn't load plugin");
		}
		/// LOG_DEBUG("couldn't load plugin module");
	}
    return EXIT_FAILURE;
}
