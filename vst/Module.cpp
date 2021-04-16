#include "Interface.h"
#include "Utility.h"

#ifdef _WIN32
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#elif defined(__APPLE__)
# include <CoreFoundation/CoreFoundation.h>
#else
# include <dlfcn.h>
#endif

#include <sstream>

/*///////////// IModule //////////////*/

// from VST3_SDK/pluginterfaces/base/fplatform.h
#ifdef _WIN32
#define PLUGIN_API __stdcall
#else
#define PLUGIN_API
#endif

#ifndef UNLOAD_MODULES
#define UNLOAD_MODULES 1
#endif

namespace vst {

#if defined(_WIN32)

class ModuleWin32 : public IModule {
 public:
    typedef bool (PLUGIN_API* InitFunc)();
    typedef bool (PLUGIN_API* ExitFunc)();
    ModuleWin32(const std::string& path){
        handle_ = LoadLibraryW(widen(path).c_str());
        if (!handle_){
            throw Error(Error::ModuleError, errorMessage(GetLastError()));
        }
        // LOG_DEBUG("loaded Win32 library " << path);
    }
    ~ModuleWin32(){
    #if UNLOAD_MODULES
        FreeLibrary(handle_);
    #endif
    }
    bool init() override {
        auto fn = getFnPtr<InitFunc>("InitDll");
        return (!fn || fn()); // init is optional
    }
    bool exit() override {
        auto fn = getFnPtr<ExitFunc>("ExitDll");
        return (!fn || fn()); // exit is optional
    }
    void * doGetFnPtr(const char *name) const override {
        return (void *)GetProcAddress(handle_, name);
    }
 private:
    HMODULE handle_;
};

#elif defined(__APPLE__)

class ModuleApple : public IModule {
 public:
    typedef bool (PLUGIN_API *InitFunc) (CFBundleRef);
    typedef bool (PLUGIN_API *ExitFunc) ();
    ModuleApple(const std::string& path){
        // Create a fullPath to the bundle
        // kudos to http://teragonaudio.com/article/How-to-make-your-own-VST-host.html
        int err = 0;
        auto pluginPath = CFStringCreateWithCString(NULL, path.c_str(), kCFStringEncodingUTF8);
        auto bundleUrl = CFURLCreateWithFileSystemPath(NULL, pluginPath, kCFURLPOSIXPathStyle, true);
        if (pluginPath) {
            CFRelease(pluginPath);
        }
        if (bundleUrl){
            bundle_ = CFBundleCreate(NULL, bundleUrl); // Open the bundle
            err = errno; // immediately catch possible error
            CFRelease(bundleUrl);
        } else {
            throw Error(Error::ModuleError, "couldn't create bundle URL");
        }

        if (bundle_){
            // force loading!
            if (!CFBundleLoadExecutable(bundle_)){
                err = errno;
                throw Error(Error::ModuleError, err ? errorMessage(err) : "CFBundleLoadExecutable failed");
            }
        } else {
            std::stringstream ss;
            ss << "couldn't open bundle (" << errorMessage(err) << ")";
            throw Error(Error::ModuleError, ss.str());
        }
        // LOG_DEBUG("loaded macOS bundle " << path);
    }
    ~ModuleApple(){
    #if UNLOAD_MODULES
        CFRelease(bundle_);
    #endif
    }
    bool init() override {
        auto fn = getFnPtr<InitFunc>("bundleEntry");
        return (fn && fn(bundle_)); // init is mandatory
    }
    bool exit() override {
        auto fn = getFnPtr<ExitFunc>("bundleExit");
        return (fn && fn()); // exit is mandatory
    }
    void * doGetFnPtr(const char *name) const override {
        auto str = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
        auto fnPtr = CFBundleGetFunctionPointerForName(bundle_, str);
        if (str) CFRelease(str);
        return fnPtr;
    }
 private:
    CFBundleRef bundle_;
};

#else

class ModuleSO : public IModule {
 public:
    typedef bool (PLUGIN_API* InitFunc) (void*);
    typedef bool (PLUGIN_API* ExitFunc) ();
    ModuleSO(const std::string& path){
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_DEEPBIND);
        if (!handle_) {
            auto error = dlerror();
            std::stringstream ss;
            ss << (error ? error : "dlopen failed");
            throw Error(Error::ModuleError, ss.str());
        }
        // LOG_DEBUG("loaded dynamic library " << path);
    }
    ~ModuleSO(){
    #if UNLOAD_MODULES
        dlclose(handle_);
    #endif
    }
    // NOTE: actually, init() and exit() should be mandatory but some plugins don't care...
    bool init() override {
        auto fn = getFnPtr<InitFunc>("ModuleEntry");
        return (!fn || fn(handle_)); // init is optional
    }
    bool exit() override {
        auto fn = getFnPtr<ExitFunc>("ModuleExit");
        return (!fn || fn()); // exit is optional
    }
    void * doGetFnPtr(const char *name) const override {
        return dlsym(handle_, name);
    }
 private:
    void *handle_;
};

#endif

// exceptions can propogate from the Module's constructor!
std::unique_ptr<IModule> IModule::load(const std::string& path){
#if defined(_WIN32)
    return std::make_unique<ModuleWin32>(path);
#elif defined __APPLE__
    return std::make_unique<ModuleApple>(path);
#else
    return std::make_unique<ModuleSO>(path);
#endif
}

} // vst
