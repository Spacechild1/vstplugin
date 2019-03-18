#include "vstplugin~.h"

// map paths to plugin info (if the plugin has been successfully probed)
static PluginInfoDict pluginInfoDict;
PluginInfoDict& getPluginInfoDict(){
    return pluginInfoDict;
}
// map plugin names (symbols) to paths, added via 'search'
static PluginPathDict pluginPathDict;
PluginPathDict& getPluginPathDict(){
    return pluginPathDict;
}

bool doProbePlugin(const std::string& path, VSTPluginInfo& info){
    auto result = probePlugin(path, info);
    if (result == VSTProbeResult::success) {
        verbose(PD_DEBUG, "probing '%s' ... ok!", path.c_str());
    }
    else if (result == VSTProbeResult::fail) {
        verbose(PD_DEBUG, "probing '%s' ... failed!", path.c_str());
    }
    else if (result == VSTProbeResult::crash) {
        verbose(PD_NORMAL, "probing '%s' ... crashed!", path.c_str());
    }
    else if (result == VSTProbeResult::error) {
        verbose(PD_ERROR, "probing '%s' ... error!", path.c_str());
    }
    return result == VSTProbeResult::success;
}

void doSearch(const char *path, t_vstplugin *x){
    int count = 0;
    std::vector<t_symbol *> pluginList;
    verbose(PD_NORMAL, "searching in '%s' ...", path);
    searchPlugins(path, [&](const std::string& absPath, const std::string& relPath){
        t_symbol *pluginName = nullptr;
        std::string pluginPath = absPath;
        sys_unbashfilename(&pluginPath[0], &pluginPath[0]);
            // probe plugin (if necessary)
        auto it = pluginInfoDict.find(pluginPath);
        if (it == pluginInfoDict.end()){
            VSTPluginInfo info;
            if (doProbePlugin(pluginPath, info)){
                pluginInfoDict[pluginPath] = info;
                pluginName = gensym(info.name.c_str());
            }
        } else {
            // already probed, just post the path
            verbose(PD_DEBUG, "%s", pluginPath.c_str());
            pluginName = gensym(it->second.name.c_str());
        }
        if (pluginName){
            // add to global dict
            pluginPathDict[pluginName] = pluginPath;
            // safe path for later
            if (x){
                pluginList.push_back(pluginName);
            }
            count++;
        }
    });
    verbose(PD_NORMAL, "found %d plugins.", count);
    if (x){
        // sort plugin names alphabetically and case independent
        std::sort(pluginList.begin(), pluginList.end(), [](const auto& lhs, const auto& rhs){
            std::string s1 = lhs->s_name;
            std::string s2 = rhs->s_name;
            for (auto& c : s1) { c = std::tolower(c); }
            for (auto& c : s2) { c = std::tolower(c); }
            return strcmp(s1.c_str(), s2.c_str()) < 0;
        });
        for (auto& plugin : pluginList){
            t_atom msg;
            SETSYMBOL(&msg, plugin);
            outlet_anything(x->x_messout, gensym("plugin"), 1, &msg);
        }
    }
}

// called by [vstsearch]
extern "C" {
    void vst_search(void){
        for (auto& path : getDefaultSearchPaths()){
            doSearch(path.c_str());
        }
    }
}
