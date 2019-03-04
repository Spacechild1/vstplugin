#include "VSTPluginInterface.h"

int main(int argc, const char *argv[]){
    if (argc >= 2){
        auto plugin = loadVSTPlugin(argv[1], true);
        if (plugin) {
            if (argc >= 3){
                VstPluginInfo info;
                info.set(*plugin);
                std::ofstream file(argv[2], std::ios::binary);
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
