#include "VSTPlugin.h"
#include "VST2Plugin.h"

#include <iostream>
#include <thread>
#include <cstring>

#if _WIN32
# include <windows.h>
#endif
#ifdef DL_OPEN
# include <dlfcn.h>
#endif

#if defined __APPLE__
# include <CoreFoundation/CoreFoundation.h>
# include <mach-o/dyld.h>
# include <unistd.h>
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
#ifdef _WIN32
    HMODULE handle = LoadLibraryW(widen(path).c_str());
    if (handle){
        mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "VSTPluginMain"));
        if (!mainEntryPoint){
            mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "main"));
        }
    } else {
        std::cout << "loadVSTPlugin: couldn't open " << path << "" << std::endl;
    }
#elif defined __APPLE__
    // Create a path to the bundle
    // kudos to http://teragonaudio.com/article/How-to-make-your-own-VST-host.html
    CFStringRef pluginPathStringRef = CFStringCreateWithCString(NULL,
        path.c_str(), kCFStringEncodingUTF8);
    CFURLRef bundleUrl = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
        pluginPathStringRef, kCFURLPOSIXPathStyle, true);
    CFBundleRef bundle = nullptr;
    if(bundleUrl) {
      // Open the bundle
      bundle = CFBundleCreate(kCFAllocatorDefault, bundleUrl);
      if(!bundle) {
        std<<cout << "loadVSTPlugin: couldn't create bundle reference for " path << std::endl;
        CFRelease(pluginPathStringRef);
        CFRelease(bundleUrl);
      }
    }
    if (bundle) {
      vstPluginFuncPtr mainEntryPoint = NULL;
      mainEntryPoint = (vstPluginFuncPtr)CFBundleGetFunctionPointerForName(bundle,
          CFSTR("VSTPluginMain"));
      // VST plugins previous to the 2.4 SDK used main_macho for the entry point name
      if(!mainEntryPoint) {
          mainEntryPoint = (vstPluginFuncPtr)CFBundleGetFunctionPointerForName(bundle,
              CFSTR("main_macho"));
      }
    }
#elif DL_OPEN
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
        std::cout << "loadVSTPlugin: not a VST plugin!" << std::endl;
        return nullptr;
    }
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
    #elif defined(USE_WINDOW_FOO)
        win = createFoo(plugin);
    #endif
        return win;
    }
}

std::string makeVSTPluginFilePath(const std::string& name){
    auto ext = name.find_last_of('.');
#ifdef _WIN32
    // myplugin -> myplugin.dll
    if (ext == std::string::npos || name.find(".dll", ext) == std::string::npos){
        return name + ".dll";
    }
#elif defined(__linux__)
    // myplugin -> myplugin.so
    if (ext == std::string::npos || name.find(".so", ext) == std::string::npos){
        return name + ".so";
    }
#elif defined(__APPLE__)
    // myplugin -> myplugin.vst/Contents/MacOS/myplugin
    if (ext == std::string::npos || name.find(".vst", ext) == std::string::npos){
        auto slash = name.find_last_of('/');
        std::string basename = (slash == std::string::npos) ? name : name.substr(slash+1);
        return name + ".vst/Contents/MacOS/" + basename;
    }
#endif
    // return unchanged
    return name;
}
