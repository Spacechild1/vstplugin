#include "VSTPluginInterface.h"
#include <windows.h>

std::wstring widen(const std::string& s);
std::string shorten(const std::wstring& s);

int wmain(int argc, const wchar_t *argv[]){
    if (argc >= 2){
        auto plugin = loadVSTPlugin(shorten(argv[1]), true);
        if (plugin) {
            if (argc >= 3){
                VstPluginInfo info;
                info.set(*plugin);
                // there's no way to open a fstream with a wide character path...
                // (the C++17 standard allows filesystem::path but this isn't widely available yet)
                // for now let's assume temp paths are always ASCII. LATER fix this!
                std::ofstream file(shorten(argv[2]), std::ios::binary);
                if (file.is_open()) {
                    info.serialize(file);
                }
            }
            freeVSTPlugin(plugin);
            return 1;
        }
    }
    return 0;
}

// hack
void SCLog(const std::string&){}
