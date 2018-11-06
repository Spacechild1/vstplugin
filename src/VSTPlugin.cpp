#include "VSTPlugin.h"
#include "VST2Plugin.h"

#include <iostream>
#include <thread>

#if _WIN32
# include <windows.h>
#endif
#ifdef DL_OPEN
# include <dlfcn.h>
#endif

#if _WIN32
static std::wstring widen(const std::string& s){
    if (s.empty()){
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), s.size(), NULL, 0);
    std::wstring buf;
    buf.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), s.size(), &buf[0], n);
    return buf;
}
static std::string shorten(const std::wstring& s){
    if (s.empty()){
        return std::string();
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), s.size(), NULL, 0, NULL, NULL);
    std::string buf;
    buf.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), s.size(), &buf[0], n, NULL, NULL);
    return buf;
}
#endif


/*//////////// VST PLUGIN ///////////*/

VSTPlugin::VSTPlugin(const std::string& path)
    : path_(path)
{}

void VSTPlugin::createWindow(){
    if (!hasEditor()){
        std::cout << "plugin doesn't have editor!" << std::endl;
        return;
    }
    // check if editor is already open
    if (win_ && win_->isRunning()){
        win_->restore();
    } else {
        win_ = std::unique_ptr<IVSTWindow>(VSTWindowFactory::create(*this));
    }
}

void VSTPlugin::destroyWindow(){
    if (!hasEditor()){
        std::cout << "plugin doesn't have editor!" << std::endl;
        return;
    }
    win_ = nullptr;
}

// protected
std::string VSTPlugin::getBaseName() const {
    auto sep = path_.find_last_of("\\/");
    auto dot = path_.find_last_of('.');
    if (sep == std::string::npos){
        sep = -1;
    }
    if (dot == std::string::npos){
        dot = path_.size();
    }
    return path_.substr(sep + 1, dot - sep - 1);
}


IVSTPlugin* loadVSTPlugin(const std::string& path){
    AEffect *plugin = nullptr;
    vstPluginFuncPtr mainEntryPoint = NULL;
#if _WIN32
    if (NULL == mainEntryPoint) {
        HMODULE handle = nullptr;
        auto ext = path.find_last_of('.');
        if (ext != std::string::npos &&
                ((path.find(".dll", ext) != std::string::npos)
                || path.find(".DLL", ext) != std::string::npos)){
            handle = LoadLibraryW(widen(path).c_str());
        }
        if (!handle) { // add extension
            wchar_t buf[MAX_PATH];
            snwprintf(buf, MAX_PATH, L"%S.dll", widen(path).c_str());
            handle = LoadLibraryW(buf);
        }

        if (handle){
            mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "VSTPluginMain"));
            if (!mainEntryPoint){
                mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "main"));
            }
        } else {
            std::cout << "loadVSTPlugin: couldn't open " << path << "" << std::endl;
        }
    }
#endif
#ifdef DL_OPEN
    if (NULL == mainEntryPoint) {
        void *handle = dlopen(path.c_str(), RTLD_NOW);
        dlerror();
        if(handle) {
            mainEntryPoint = (vstPluginFuncPtr)(dlsym(handle, "VSTPluginMain"));
            if (!mainEntryPoint){
                mainEntryPoint = (vstPluginFuncPtr)(dlsym(handle, "main"));
            }
        } else {
            std::cout << "loadVSTPlugin: couldn't dlopen " << path << "" << std::endl;
        }
    }
#endif

    if (!mainEntryPoint){
        std::cout << "loadVSTPlugin: couldn't find entry point in VST plugin" << std::endl;
        return nullptr;
    }
    plugin = mainEntryPoint(&VST2Plugin::hostCallback);
    if (!plugin){
        std::cout << "loadVSTPlugin: couldn't initialize plugin" << std::endl;
        return nullptr;
    }
    if (plugin->magic != kEffectMagic){
        std::cout << "loadVSTPlugin: bad magic number!" << std::endl;
        return nullptr;
    }
    std::cout << "loadVSTPlugin: successfully loaded plugin" << std::endl;
    return new VST2Plugin(plugin, path);
}

void freeVSTPlugin(IVSTPlugin *plugin){
    if (plugin){
        delete plugin;
    }
}

namespace VSTWindowFactory {
#ifdef _WIN32
    IVSTWindow* createWin32(IVSTPlugin& plugin);
#endif
#ifdef USE_WINDOW_FOO
    IVSTWindow* createFoo(IVSTPlugin& plugin);
#endif

    IVSTWindow* create(IVSTPlugin& plugin){
        IVSTWindow *win = nullptr;
    #ifdef _WIN32
        win = createWin32(plugin);
    #endif
    #ifdef USE_WINDOW_FOO
        win = createFoo(plugin);
    #endif
        return win;
    }
}

