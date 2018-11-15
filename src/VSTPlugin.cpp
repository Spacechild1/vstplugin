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


IVSTPlugin* loadVSTPlugin(const std::string& path){
    AEffect *plugin = nullptr;
    vstPluginFuncPtr mainEntryPoint = nullptr;
    bool openedlib = false;
#ifdef _WIN32
    if(!mainEntryPoint) {
        HMODULE handle = LoadLibraryW(widen(path).c_str());
        if (handle) {
            openedlib = true;
            mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "VSTPluginMain"));
            if (!mainEntryPoint){
                mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "main"));
            }
            if (!mainEntryPoint){
                FreeLibrary(handle);
            }
        } else {
            std::cout << "loadVSTPlugin: couldn't open " << path << "" << std::endl;
        }
    }
#endif
#if defined __APPLE__
    if(!mainEntryPoint) {
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
                std::cout << "loadVSTPlugin: couldn't create bundle reference for " << path << std::endl;
            }
        }
        if (bundle) {
            openedlib = true;
            mainEntryPoint = (vstPluginFuncPtr)CFBundleGetFunctionPointerForName(bundle,
                CFSTR("VSTPluginMain"));
                // VST plugins previous to the 2.4 SDK used main_macho for the entry point name
            if(!mainEntryPoint) {
                mainEntryPoint = (vstPluginFuncPtr)CFBundleGetFunctionPointerForName(bundle,
                    CFSTR("main_macho"));
            }
            if (!mainEntryPoint){
                CFRelease( bundle );
            }
        }
        if (pluginPathStringRef)
            CFRelease(pluginPathStringRef);
        if (bundleUrl)
            CFRelease(bundleUrl);
    }
#endif
#if DL_OPEN
    if(!mainEntryPoint) {
        void *handle = dlopen(path.c_str(), RTLD_NOW);
        dlerror();
        if(handle) {
            openedlib = true;
            mainEntryPoint = (vstPluginFuncPtr)(dlsym(handle, "VSTPluginMain"));
            if (!mainEntryPoint){
                mainEntryPoint = (vstPluginFuncPtr)(dlsym(handle, "main"));
            }
            if (!mainEntryPoint){
                dlclose(handle);
            }
        } else {
            std::cout << "loadVSTPlugin: couldn't dlopen " << path << "" << std::endl;
        }
    }
#endif

    if (!openedlib) // we already printed an error if finding/opening the plugin filed
        return nullptr;

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
    IVSTWindow* createWin32();
    void initializeWin32();
#endif
#ifdef USE_X11
    void initializeX11();
    IVSTWindow* createX11(void *display);
#endif
        // initialize
    void initialize(){
#ifdef _WIN32
        initializeWin32();
#endif
#ifdef __linux__
        initializeX11();
#endif
    }
        // create
    IVSTWindow* create(void *context){
        IVSTWindow *win = nullptr;
#ifdef _WIN32
        win = createWin32();
#elif defined(USE_X11)
        win = createX11(context);
#endif
        return win;
    }
}

std::string makeVSTPluginFilePath(const std::string& name){
    auto dot = name.find_last_of('.');
#ifdef _WIN32
    const char *ext = ".dll";
#elif defined(__linux__)
    const char *ext = ".so";
#elif defined(__APPLE__)
    const char *ext = ".vst";
#else
    std::cout << "makeVSTPluginFilePath: unknown platform!" << std::endl;
    return name;
#endif
    if (dot == std::string::npos || name.find(ext, dot) == std::string::npos){
        return name + ext;
    } else {
        return name; // already has proper extension, return unchanged
    }
}
