#include "Interface.h"
#include "Utility.h"
#if USE_VST2
 #include "VST2Plugin.h"
#endif
#if USE_VST3
 #include "VST3Plugin.h"
#endif

#include <unordered_set>
#include <unordered_map>
#include <stdlib.h>
#include <cstring>
#include <sstream>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

#ifdef _WIN32
# include <Windows.h>
# include <process.h>
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else 
// just because of Clang on macOS not shipping <experimental/filesystem>...
# include <dirent.h>
# include <unistd.h>
# include <strings.h>
# include <sys/wait.h>
# include <sys/stat.h>
# include <sys/types.h>
// for probing (and dlopen)
# include <dlfcn.h>
# include <stdio.h>
#endif

#ifdef __APPLE__
# include <CoreFoundation/CoreFoundation.h>
# include <mach-o/dyld.h>
# include <unistd.h>
#endif

namespace vst {

// forward declarations to avoid including the header files 
// (creates troubles with Cocoa)
#ifdef _WIN32
namespace Win32 {
namespace UIThread {
    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
#if HAVE_UI_THREAD
    bool check();
#else
    void poll();
#endif
} // UIThread
} // Win32
#elif defined(__APPLE__)
namespace Cocoa {
namespace UIThread {
    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
#if HAVE_UI_THREAD
    bool check();
#else
    void poll();
#endif
} // UIThread
} // Cocoa
#elif defined(USE_X11)
namespace X11 {
namespace UIThread {
    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
#if HAVE_UI_THREAD
    bool check();
#else
    void poll();
#endif
} // UIThread
} // X11
#endif

/*////////////////////// platform ///////////////////*/

#ifdef _WIN32
std::wstring widen(const std::string& s){
    if (s.empty()){
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), s.size(), NULL, 0);
    std::wstring buf;
    buf.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), s.size(), &buf[0], n);
    return buf;
}
std::string shorten(const std::wstring& s){
    if (s.empty()){
        return std::string();
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), s.size(), NULL, 0, NULL, NULL);
    std::string buf;
    buf.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), s.size(), &buf[0], n, NULL, NULL);
    return buf;
}

static HINSTANCE hInstance = 0;

static std::wstring getDirectory(){
    wchar_t wpath[MAX_PATH+1];
    if (GetModuleFileNameW(hInstance, wpath, MAX_PATH) > 0){
        wchar_t *ptr = wpath;
        int pos = 0;
        while (*ptr){
            if (*ptr == '\\'){
                pos = (ptr - wpath);
            }
            ++ptr;
        }
        wpath[pos] = 0;
        // LOG_DEBUG("dll directory: " << shorten(wpath));
        return std::wstring(wpath);
    } else {
        LOG_ERROR("couldn't get module file name");
        return std::wstring();
    }
}

extern "C" {
    BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved){
        if (fdwReason == DLL_PROCESS_ATTACH){
            hInstance = hinstDLL;
        }
        return TRUE;
    }
}

std::string expandPath(const char *path) {
    wchar_t buf[MAX_PATH];
    ExpandEnvironmentStringsW(widen(path).c_str(), buf, MAX_PATH);
    return shorten(buf);
}

#else
std::string expandPath(const char *path) {
    // only expands ~ to home directory so far
    if (path && *path == '~') {
        const char *home = getenv("HOME");
        if (home) {
            return std::string(home) + std::string(path + 1);
        }
    }
    return path;
}
#endif

bool pathExists(const std::string& path){
#ifdef _WIN32
    std::error_code e;
    return fs::exists(widen(path), e);
#else
    struct stat stbuf;
    return stat(path.c_str(), &stbuf) == 0;
#endif
}

bool isDirectory(const std::string& path){
#ifdef _WIN32
    std::error_code e;
    return fs::is_directory(widen(path), e);
#else
    struct stat stbuf;
    return (stat(path.c_str(), &stbuf) == 0) && S_ISDIR(stbuf.st_mode);
#endif
}

bool removeFile(const std::string& path){
#ifdef _WIN32
    std::error_code e;
    fs::remove(widen(path), e);
    if (e){
        LOG_ERROR(e.message());
        return false;
    } else {
        return true;
    }
#else
    return remove(path.c_str()) == 0;
#endif
}

bool createDirectory(const std::string& dir){
#ifdef _WIN32
    std::error_code err;
    return fs::create_directory(widen(dir), err);
#else
    int result = mkdir(dir.c_str(), ACCESSPERMS);
    if (result == 0){
        // force correct permission with chmod() in case the umask has been set
        // to the wrong value. setting/unsetting the umask is not thread safe...
        if (chmod(dir.c_str(), ACCESSPERMS) == 0){
            return true;
        } else {
            LOG_ERROR("chmod failed!");
        }
    }
    return false;
#endif
}

std::string fileName(const std::string& path){
#ifdef _WIN32
    auto pos = path.find_last_of("/\\");
#else
    auto pos = path.find_last_of('/');
#endif
    if (pos != std::string::npos){
        return path.substr(pos+1);
    } else {
        return path;
    }
}

std::string getTmpDirectory(){
#ifdef _WIN32
    wchar_t tmpDir[MAX_PATH + 1];
    auto tmpDirLen = GetTempPathW(MAX_PATH, tmpDir);
    if (tmpDirLen > 0){
        return shorten(tmpDir);
    } else {
        return std::string{};
    }
#else
    for (auto var : { "TMPDIR", "TMP", "TEMP", "TEMPDIR"}){
        auto dir = getenv(var);
        if (dir) return dir;
    }
    return "/tmp"; // fallback
#endif
}

/*////////////////////// search ///////////////////////*/

static std::vector<const char *> platformExtensions = {
#if USE_VST2
#ifdef __APPLE__
    ".vst"
#elif defined(_WIN32)
    ".dll"
#else
    ".so"
#endif
#endif // VST2
#if USE_VST2 && USE_VST3
    ,
#endif
#if USE_VST3
    ".vst3"
#endif // VST3
};

const std::vector<const char *>& getPluginExtensions() {
    return platformExtensions;
}

const std::string& getBundleBinaryPath(){
    static std::string path =
#if defined(_WIN32)
#ifdef _WIN64
        "Contents/x86_64-win";
#else // WIN32
        "Contents/x86-win";
#endif
#elif defined(__APPLE__)
        "Contents/MacOS";
#else // Linux
#if defined(__i386__)
        "Contents/i386-linux";
#elif defined(__x86_64__)
        "Contents/x86_64-linux";
#else
        ""; // figure out what to do with all the ARM versions...
#endif
#endif
    return path;
}

#ifdef _WIN32
#ifdef _WIN64 // 64 bit
#define PROGRAMFILES "%ProgramFiles%\\"
#else // 32 bit
#define PROGRAMFILES "%ProgramFiles(x86)%\\"
#endif
#endif // WIN32

static std::vector<const char *> defaultSearchPaths = {
    /*////// VST2 ////*/
#if USE_VST2
    // macOS
#ifdef __APPLE__
	"~/Library/Audio/Plug-Ins/VST", "/Library/Audio/Plug-Ins/VST"
#endif
    // Windows
#ifdef _WIN32
    PROGRAMFILES "VSTPlugins", PROGRAMFILES "Steinberg\\VSTPlugins",
    PROGRAMFILES "Common Files\\VST2", PROGRAMFILES "Common Files\\Steinberg\\VST2"
#endif
    // Linux
#ifdef __linux__
    "~/.vst", "/usr/local/lib/vst", "/usr/lib/vst"
#endif
#endif // VST2
#if USE_VST2 && USE_VST3
    ,
#endif
    /*////// VST3 ////*/
#if USE_VST3
    // macOS
#ifdef __APPLE__
    "~/Library/Audio/Plug-Ins/VST3", "/Library/Audio/Plug-Ins/VST3"
#endif
    // Windows
#ifdef _WIN32
    PROGRAMFILES "Common Files\\VST3"
#endif
    // Linux
#ifdef __linux__
    "~/.vst3", "/usr/local/lib/vst3", "/usr/lib/vst3"
#endif
#endif // VST3
};

#ifdef PROGRAMFILES
#undef PROGRAMFILES
#endif

// get "real" default search paths
const std::vector<std::string>& getDefaultSearchPaths() {
    // thread safe since C++11
    static const struct SearchPaths {
        SearchPaths(){
            for (auto& path : defaultSearchPaths) {
                list.push_back(expandPath(path));
            }
        }
        std::vector<std::string> list; // expanded search paths
    } searchPaths;
    return searchPaths.list;
}

#ifndef _WIN32
// helper function
static bool isDirectory(const std::string& dir, dirent *entry){
    // we don't count "." and ".."
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0){
        return false;
    }
#ifdef _DIRENT_HAVE_D_TYPE
    // some filesystems don't support d_type, also we want to follow symlinks
    if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_LNK){
        return (entry->d_type == DT_DIR);
    }
    else
#endif
    {
        struct stat stbuf;
        std::string path = dir + "/" + entry->d_name;
        if (stat(path.c_str(), &stbuf) == 0){
            return S_ISDIR(stbuf.st_mode);
        }
    }
    return false;
}
#endif

// recursively search for a VST plugin in a directory. returns empty string on failure.
std::string find(const std::string &dir, const std::string &path){
    std::string relpath = path;
#ifdef _WIN32
    const char *ext = ".dll";
#elif defined(__APPLE__)
    const char *ext = ".vst";
#else // Linux/BSD/etc.
    const char *ext = ".so";
#endif
    if (relpath.find(".vst3") == std::string::npos && relpath.find(ext) == std::string::npos){
        relpath += ext;
    }
#ifdef _WIN32
    try {
        auto wdir = widen(dir);
        auto fpath = fs::path(widen(relpath));
        auto file = fs::path(wdir) / fpath;
        if (fs::is_regular_file(file)){
            return file.u8string(); // success
        }
        // continue recursively
        for (auto& entry : fs::recursive_directory_iterator(wdir)) {
            if (fs::is_directory(entry)){
                file = entry.path() / fpath;
                if (fs::exists(file)){
                    return file.u8string(); // success
                }
            }
        }
    } catch (const fs::filesystem_error& e) {};
    return std::string{};
#else // Unix
    std::string result;
    // force no trailing slash
    auto root = (dir.back() == '/') ? dir.substr(0, dir.size() - 1) : dir;

    std::string file = root + "/" + relpath;
    if (pathExists(file)){
        return file; // success
    }
    // continue recursively
    std::function<void(const std::string&)> searchDir = [&](const std::string& dirname) {
        DIR *directory = opendir(dirname.c_str());
        struct dirent *entry;
        if (directory){
            while (result.empty() && (entry = readdir(directory))){
                if (isDirectory(dirname, entry)){
                    std::string d = dirname + "/" + entry->d_name;
                    std::string absPath = d + "/" + relpath;
                    if (pathExists(absPath)){
                        result = absPath;
                        break;
                    } else {
                        // dive into the direcotry
                        searchDir(d);
                    }
                }
            }
            closedir(directory);
        }
    };
    searchDir(root);
    return result;
#endif
}

// recursively search a directory for VST plugins. for every plugin, 'fn' is called with the full path and base name.
void search(const std::string &dir, std::function<void(const std::string&, const std::string&)> fn) {
    // extensions
    std::unordered_set<std::string> extensions;
    for (auto& ext : platformExtensions) {
        extensions.insert(ext);
    }
    // search recursively
#ifdef _WIN32
    std::function<void(const std::wstring&)> searchDir = [&](const std::wstring& dirname){
        try {
            // LOG_DEBUG("searching in " << shorten(dirname));
            for (auto& entry : fs::directory_iterator(dirname)) {
                // check the extension
                auto ext = entry.path().extension().u8string();
                if (extensions.count(ext)) {
                    // found a VST2 plugin (file or bundle)
                    auto abspath = entry.path().u8string();
                    auto basename = entry.path().filename().u8string();
                    fn(abspath, basename);
                } else if (fs::is_directory(entry.path())){
                    // otherwise search it if it's a directory
                    searchDir(entry.path());
                }
            }
        } catch (const fs::filesystem_error& e) {
            LOG_DEBUG(e.what());
        };
    };
    searchDir(widen(dir));
#else // Unix
    // force no trailing slash
    auto root = (dir.back() == '/') ? dir.substr(0, dir.size() - 1) : dir;
    std::function<void(const std::string&)> searchDir = [&](const std::string& dirname) {
        // search alphabetically (ignoring case)
        struct dirent **dirlist;
        auto sortnocase = [](const struct dirent** a, const struct dirent **b) -> int {
            return strcasecmp((*a)->d_name, (*b)->d_name);
        };
        int n = scandir(dirname.c_str(), &dirlist, NULL, sortnocase);
        if (n >= 0) {
            for (int i = 0; i < n; ++i) {
                auto entry = dirlist[i];
                std::string name(entry->d_name);
                std::string absPath = dirname + "/" + name;
                // check the extension
                std::string ext;
                auto extPos = name.find_last_of('.');
                if (extPos != std::string::npos) {
                    ext = name.substr(extPos);
                }
                if (extensions.count(ext)) {
                    // found a VST2 plugin (file or bundle)
                    fn(absPath, name);
                }
                // otherwise search it if it's a directory
                else if (isDirectory(dirname, entry)) {
                    searchDir(absPath);
                }
                free(entry);
            }
            free(dirlist);
        }
    };
    searchDir(root);
#endif
}

/*/////////// Message Loop //////////////////*/

namespace UIThread {
    IPlugin::ptr create(const PluginInfo &info){
        IPlugin::ptr plugin;
    #ifdef _WIN32
        plugin = Win32::UIThread::create(info);
    #elif defined(__APPLE__)
        plugin = Cocoa::UIThread::create(info);
    #elif defined(USE_X11)
        plugin = X11::UIThread::create(info);
    #endif
        return plugin;
    }

    void destroy(IPlugin::ptr plugin){
    #ifdef _WIN32
        Win32::UIThread::destroy(std::move(plugin));
    #elif defined(__APPLE__)
        Cocoa::UIThread::destroy(std::move(plugin));
    #elif defined(USE_X11)
        X11::UIThread::destroy(std::move(plugin));
    #endif
    }

#if HAVE_UI_THREAD
    bool check(){
    #ifdef _WIN32
        return Win32::UIThread::check();
    #elif defined(__APPLE__)
        return Cocoa::UIThread::check();
    #elif defined(USE_X11)
        return X11::UIThread::check();
    #endif
    }
#else
    void poll(){
    #ifdef _WIN32
        Win32::UIThread::poll();
    #elif defined(__APPLE__)
        Cocoa::UIThread::poll();
    #elif defined(USE_X11)
        X11::UIThread::poll();   
    #endif
    }
#endif
}

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

#ifdef _WIN32
class ModuleWin32 : public IModule {
 public:
    typedef bool (PLUGIN_API* InitFunc)();
    typedef bool (PLUGIN_API* ExitFunc)();
    ModuleWin32(const std::string& path){
        handle_ = LoadLibraryW(widen(path).c_str());
        if (!handle_){
            auto error = GetLastError();
            std::stringstream ss;
            char buf[1000];
            buf[0] = 0;
            auto size = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error,
                                            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, sizeof(buf), NULL);
            if (size > 0){
                buf[size-1] = 0; // omit newline
            }
            ss << "LoadLibrary failed (" << error << ") - " << buf;
            throw Error(ss.str());
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
#endif

#ifdef __APPLE__
class ModuleApple : public IModule {
 public:
    typedef bool (PLUGIN_API *InitFunc) (CFBundleRef);
    typedef bool (PLUGIN_API *ExitFunc) ();
    ModuleApple(const std::string& path){
        // Create a fullPath to the bundle
        // kudos to http://teragonaudio.com/article/How-to-make-your-own-VST-host.html
        auto pluginPath = CFStringCreateWithCString(NULL, path.c_str(), kCFStringEncodingUTF8);
        auto bundleUrl = CFURLCreateWithFileSystemPath(NULL, pluginPath, kCFURLPOSIXPathStyle, true);
        if (bundleUrl) {
                // Open the bundle
            bundle_ = CFBundleCreate(NULL, bundleUrl);
        }
        if (pluginPath) CFRelease(pluginPath);
        if (bundleUrl) CFRelease(bundleUrl);
        if (!bundle_){
            throw Error("couldn't create bundle reference");
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
#endif

#if DL_OPEN
class ModuleSO : public IModule {
 public:
    typedef bool (PLUGIN_API* InitFunc) (void*);
    typedef bool (PLUGIN_API* ExitFunc) ();
    ModuleSO(const std::string& path){
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_DEEPBIND);
        auto error = dlerror();
        if (!handle_) {
            std::stringstream ss;
            ss << "dlopen failed (" << error << ") - " << strerror(error);
            throw Error(ss.str());
        }
        // LOG_DEBUG("loaded dynamic library " << path);
    }
    ~ModuleSO(){
    #if UNLOAD_MODULES
        dlclose(handle_);
    #endif
    }
    bool init() override {
        auto fn = getFnPtr<InitFunc>("ModuleEntry");
        return (fn && fn(handle_)); // init is mandatory
    }
    bool exit() override {
        auto fn = getFnPtr<ExitFunc>("ModuleExit");
        return (fn && fn()); // exit is mandatory
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
#ifdef _WIN32
    return std::make_unique<ModuleWin32>(path);
#endif
#if defined __APPLE__
    return std::make_unique<ModuleApple>(path);
#endif
#if DL_OPEN
    return std::make_unique<ModuleSO>(path);
#endif
    return std::unique_ptr<IModule>{};
}

/*///////////////////// IFactory ////////////////////////*/

IFactory::ptr IFactory::load(const std::string& path){
#ifdef _WIN32
    const char *ext = ".dll";
#elif defined(__APPLE__)
	const char *ext = ".vst";
#else // Linux/BSD/etc.
    const char *ext = ".so";
#endif
    // LOG_DEBUG("IFactory: loading " << path);
    if (path.find(".vst3") != std::string::npos){
    #if USE_VST3
        return std::make_shared<VST3Factory>(path);
    #else
        throw Error("VST3 plug-ins not supported");
    #endif
    } else {
    #if USE_VST2
        if (path.find(ext) != std::string::npos){
            return std::make_shared<VST2Factory>(path);
        } else {
            return std::make_shared<VST2Factory>(path + ext);
        }
    #else
        throw Error("VST2 plug-ins not supported");
    #endif
    }
}

// RAII class for automatic cleanup
class TmpFile : public File {
public:
    using File::File;
    ~TmpFile(){
        if (is_open()){
            close();
            // destructor must not throw!
            if (!removeFile(path_)){
                LOG_ERROR("couldn't remove tmp file!");
            };
        }
    }
};

// should probe.exe inherit file handles and print to stdout/stderr?
#define PROBE_LOG 0

// probe a plugin in a seperate process and return the info in a file
PluginInfo::Future IFactory::probePlugin(const std::string& name, int shellPluginID) {
    auto desc = std::make_shared<PluginInfo>(shared_from_this());
    // put the information we already have (might be overriden)
    desc->name = name;
    desc->path = path();
    // we pass the shell plugin ID instead of the name to probe.exe
    std::string pluginName = shellPluginID ? std::to_string(shellPluginID) : name;
    // create temp file path
    std::stringstream ss;
    ss << "/vst_" << desc.get(); // desc address should be unique as long as PluginInfos are retained.
    std::string tmpPath = getTmpDirectory() + ss.str();
    // LOG_DEBUG("temp path: " << tmpPath);
#ifdef _WIN32
    // get full path to probe exe
    std::wstring probePath = getDirectory() + L"\\probe.exe";
    /// LOG_DEBUG("probe path: " << shorten(probePath));
    // on Windows we need to quote the arguments for _spawn to handle spaces in file names.
    std::stringstream cmdLineStream;
    cmdLineStream << "probe.exe "
            << "\"" << path() << "\" "
            << "\"" << pluginName << "\" "
            << "\"" << tmpPath + "\"";
    auto cmdLine = widen(cmdLineStream.str());
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(probePath.c_str(), &cmdLine[0],
                        NULL, NULL, PROBE_LOG, 0, NULL, NULL, &si, &pi)){
        throw Error("probePlugin: couldn't spawn process!");
    }
    auto wait = [pi](){
        if (WaitForSingleObject(pi.hProcess, INFINITE) != 0){
            throw Error("probePlugin: couldn't wait for process!");
        }
        DWORD code = -1;
        if (!GetExitCodeProcess(pi.hProcess, &code)){
            throw Error("probePlugin: couldn't retrieve exit code!");
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return code;
    };
#else // Unix
    Dl_info dlinfo;
    // get full path to probe exe
    // hack: obtain library info through a function pointer (vst::search)
    if (!dladdr((void *)search, &dlinfo)) {
        throw Error("probePlugin: couldn't get module path!");
    }
    std::string modulePath = dlinfo.dli_fname;
    auto end = modulePath.find_last_of('/');
    std::string probePath = modulePath.substr(0, end) + "/probe";
    // fork
    pid_t pid = fork();
    if (pid == -1) {
        throw Error("probePlugin: fork failed!");
    }
    else if (pid == 0) {
        // child process: start new process with plugin path and temp file path as arguments.
        // we must not quote arguments to exec!
    #if !PROBE_LOG
        // disable stdout and stderr
        auto nullOut = fopen("/dev/null", "w");
        fflush(stdout);
        dup2(fileno(nullOut), STDOUT_FILENO)
        fflush(stderr);
        dup2(fileno(nullOut), STDERR_FILENO)
    #endif
        if (execl(probePath.c_str(), "probe", path().c_str(), pluginName.c_str(), tmpPath.c_str(), nullptr) < 0) {
            LOG_ERROR("probePlugin: exec failed!");
        }
        std::exit(EXIT_FAILURE);
    }
    // parent process: wait for child
    auto wait = [pid](){
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        } else {
            return -1;
        }
    };
#endif
    return [desc=std::move(desc), tmpPath=std::move(tmpPath), wait=std::move(wait)](){
        /// LOG_DEBUG("result: " << result);
        auto result = wait();
        if (result == EXIT_SUCCESS) {
            // get info from temp file
            TmpFile file(tmpPath);
            if (file.is_open()) {
                desc->deserialize(file);
                desc->probeResult = ProbeResult::success;
            }
            else {
                throw Error("probePlugin: couldn't read temp file!");
            }
        }
        else if (result == EXIT_FAILURE) {
            desc->probeResult = ProbeResult::fail;
        }
        else {
            desc->probeResult = ProbeResult::crash;
        }
        return desc;
    };
}

// for testing we don't want to load hundreds of sub plugins
// #define PLUGIN_LIMIT 50

// We probe sub-plugins asynchronously with "futures" or worker threads.
// The latter are just wrappers around futures, but we can gather results as soon as they are available.
// Both methods are about equally fast, the worker threads just look more responsive.
#define PROBE_FUTURES 8 // number of futures to wait for
#define PROBE_THREADS 8 // number of worker threads (0: use futures instead of threads)

#if 0
static std::mutex gLogMutex;
#define DEBUG_THREAD(x) do { gLogMutex.lock(); LOG_DEBUG(x); gLogMutex.unlock(); } while (false)
#else
#define DEBUG_THREAD(x)
#endif

std::vector<PluginInfo::ptr> IFactory::probePlugins(
        const ProbeList& pluginList, ProbeCallback callback, bool& valid){
    // shell plugin!
    int numPlugins = pluginList.size();
    std::vector<PluginInfo::ptr> results;
#ifdef PLUGIN_LIMIT
    numPlugins = std::min<int>(numPlugins, PLUGIN_LIMIT);
#endif
#if !PROBE_THREADS
    /// LOG_DEBUG("numPlugins: " << numPlugins);
    std::vector<std::tuple<int, std::string, PluginInfo::Future>> futures;
    int i = 0;
    while (i < numPlugins){
        futures.clear();
        // probe the next n plugins
        int n = std::min<int>(numPlugins - i, PROBE_FUTURES);
        for (int j = 0; j < n; ++j, ++i){
            auto& pair = pluginList[i];
            auto& name = pair.first;
            auto& id = pair.second;
            try {
                /// LOG_DEBUG("probing '" << pair.first << "'");
                futures.emplace_back(i, name, probePlugin(name, id));
            } catch (const Error& e){
                // should we rather propagate the error and break from the loop?
                LOG_ERROR("couldn't probe '" << name << "': " << e.what());
            }
        }
        // collect results
        for (auto& tup : futures){
            int index;
            std::string name;
            PluginInfo::Future future;
            std::tie(index, name, future) = tup;
            try {
                auto plugin = future(); // wait for process
                results.push_back(plugin);
                // factory is valid if contains at least 1 valid plugin
                if (plugin->valid()){
                    valid = true;
                }
                if (callback){
                    callback(*plugin, index, numPlugins);
                }
            } catch (const Error& e){
                // should we rather propagate the error and break from the loop?
                LOG_ERROR("couldn't probe '" << name << "': " << e.what());
            }
        }
    }
#else
    DEBUG_THREAD("numPlugins: " << numPlugins);
    auto next = pluginList.begin();
    auto end = next + numPlugins;
    int count = 0;
    std::deque<std::tuple<int, std::string, PluginInfo::ptr, Error>> resultQueue;

    std::mutex mutex;
    std::condition_variable cond;
    int numThreads = std::min<int>(numPlugins, PROBE_THREADS);
    std::vector<std::thread> threads;
    // thread function
    auto threadFun = [&](int i){
        std::unique_lock<std::mutex> lock(mutex);
        while (next != end){
            auto it = next++;
            auto& name = it->first;
            auto& id = it->second;
            lock.unlock();
            try {
                DEBUG_THREAD("probing '" << name << "'");
                auto plugin = probePlugin(name, id)();
                lock.lock();
                resultQueue.emplace_back(count++, name, plugin, Error{});
                DEBUG_THREAD("thread " << i << ": probed " << name);
            } catch (const Error& e){
                lock.lock();
                resultQueue.emplace_back(count++, name, nullptr, e);
            }
            cond.notify_one();
        }
        DEBUG_THREAD("worker thread " << i << " finished");
    };
    // spawn worker threads
    for (int j = 0; j < numThreads; ++j){
        threads.push_back(std::thread(threadFun, j));
    }
    // collect results
    std::unique_lock<std::mutex> lock(mutex);
    while (true) {
        // process available data
        while (resultQueue.size() > 0){
            int index;
            std::string name;
            PluginInfo::ptr plugin;
            Error e;
            std::tie(index, name, plugin, e) = resultQueue.front();
            resultQueue.pop_front();
            lock.unlock();

            if (plugin){
                results.push_back(plugin);
                DEBUG_THREAD("got plugin " << plugin->name << " (" << (index + 1) << " of " << numPlugins << ")");
                // factory is valid if contains at least 1 valid plugin
                if (plugin->valid()){
                    valid = true;
                }
                if (callback){
                    callback(*plugin, index, numPlugins);
                }
            } else {
                // should we rather propagate the error and break from the loop?
                DEBUG_THREAD("couldn't probe '" << name << "': " << e.what());
            }
            lock.lock();
        }
        if (count < numPlugins) {
            DEBUG_THREAD("wait...");
            cond.wait(lock); // wait for more
        }
        else {
            break; // done
        }
    }

    lock.unlock(); // !
    DEBUG_THREAD("exit loop");
    // join worker threads
    for (auto& thread : threads){
        if (thread.joinable()){
            thread.join();
        }
    }
    DEBUG_THREAD("all worker threads joined");
#endif
    return results;
}

void IFactory::probe(ProbeCallback callback){
    probeAsync()(std::move(callback));
}

/*///////////////////// PluginInfo /////////////////////*/

PluginInfo::PluginInfo(const std::shared_ptr<const IFactory>& factory)
    : path(factory->path()), factory_(factory) {}

IPlugin::ptr PluginInfo::create() const {
    std::shared_ptr<const IFactory> factory = factory_.lock();
    return factory ? factory->create(name) : nullptr;
}

void PluginInfo::setUniqueID(int _id){
    type_ = PluginType::VST2;
    char buf[9];
    // should we write in little endian?
    snprintf(buf, sizeof(buf), "%08X", _id);
    buf[8] = 0;
    uniqueID = buf;
}

void PluginInfo::setUID(const char *uid){
    type_ = PluginType::VST3;
    char buf[33];
    for (int i = 0; i < 16; ++i){
        snprintf(buf + (i * 2), 3, "%02X", uid[i]);
    }
    buf[32] = 0;
    uniqueID = buf;
}

/// .ini file structure for each plugin:
///
/// [plugin]
/// path=<string>
/// name=<string>
/// vendor=<string>
/// category=<string>
/// version=<string>
/// sdkversion=<string>
/// id=<int>
/// inputs=<int>
/// outputs=<int>
/// flags=<int>
/// [parameters]
/// n=<int>
/// name,label
/// name,label
/// ...
/// [programs]
/// n=<int>
/// <program0>
/// <program1>
/// <program2>
/// ...

static std::string bashString(std::string name){
    // replace "forbidden" characters
    for (auto& c : name){
        switch (c){
        case ',':
        case '\n':
        case '\r':
            c = '_';
            break;
        default:
            break;
        }
    }
    return name;
}

#define toHex(x) std::hex << (x) << std::dec

void PluginInfo::serialize(std::ostream& file) const {
    file << "[plugin]\n";
    file << "id=" << uniqueID << "\n";
    file << "path=" << path << "\n";
    file << "name=" << name << "\n";
    file << "vendor=" << vendor << "\n";
    file << "category=" << category << "\n";
    file << "version=" << version << "\n";
    file << "sdkversion=" << sdkVersion << "\n";
    file << "inputs=" << numInputs << "\n";
    if (numAuxInputs > 0){
        file << "auxinputs=" << numAuxInputs << "\n";
    }
    file << "outputs=" << numOutputs << "\n";
    if (numAuxOutputs > 0){
        file << "auxoutputs=" << numAuxOutputs << "\n";
    }
    file << "flags=" << toHex(flags) << "\n";
#if USE_VST3
    if (programChange != NoParamID){
        file << "pgmchange=" << toHex(programChange) << "\n";
    }
    if (bypass != NoParamID){
        file << "bypass=" << toHex(bypass) << "\n";
    }
#endif
    // parameters
    file << "[parameters]\n";
    file << "n=" << parameters.size() << "\n";
    for (auto& param : parameters) {
        file << bashString(param.name) << "," << param.label << "," << toHex(param.id) << "\n";
	}
    // programs
    file << "[programs]\n";
    file << "n=" << programs.size() << "\n";
	for (auto& pgm : programs) {
        file << pgm << "\n";
	}
#if USE_VST2
    // shell plugins (only used for probe.exe)
    if (!shellPlugins.empty()){
        file << "[shell]\n";
        file << "n=" << (int)shellPlugins.size() << "\n";
        for (auto& shell : shellPlugins){
            file << shell.name << "," << shell.id << "\n";
        }
    }
#endif
}

static std::string ltrim(std::string str){
    auto pos = str.find_first_not_of(" \t");
    if (pos != std::string::npos && pos > 0){
        return str.substr(pos);
    } else {
        return str;
    }
}

static std::string rtrim(std::string str){
    auto pos = str.find_last_not_of(" \t") + 1;
    if (pos != std::string::npos && pos < str.size()){
        return str.substr(0, pos);
    } else {
        return str;
    }
}

bool isComment(const std::string& line){
    auto c = line.front();
    return c == ';' || c == '#';
}

bool getLine(std::istream& stream, std::string& line){
    std::string temp;
    while (std::getline(stream, temp)){
        if (!temp.empty() && !isComment(temp)){
            line = std::move(temp);
            return true;
        }
    }
    return false;
}

int getCount(const std::string& line){
    auto pos = line.find('=');
    if (pos == std::string::npos){
        throw Error("missing '=' after key: " + line);
    }
    try {
        return std::stol(line.substr(pos + 1));
    }
    catch (...){
        throw Error("expected number after 'n='");
    }
}

static void getKeyValuePair(const std::string& line, std::string& key, std::string& value){
    auto pos = line.find('=');
    if (pos == std::string::npos){
        throw Error("missing '=' after key: " + line);
    }
    key = rtrim(line.substr(0, pos));
    value = ltrim(line.substr(pos + 1));
}

std::vector<std::string> splitString(const std::string& str, char sep){
    std::vector<std::string> result;
    auto pos = 0;
    while (true){
        auto newpos = str.find(sep, pos);
        if (newpos != std::string::npos){
            int len = newpos - pos;
            result.push_back(str.substr(pos, len));
            pos = newpos + 1;
        } else {
            result.push_back(str.substr(pos)); // remaining string
            break;
        }
    }
    return result;
}

void parseArg(int32_t& lh, const std::string& rh){
    lh = std::stol(rh); // decimal
}

void parseArg(uint32_t& lh, const std::string& rh){
    lh = std::stol(rh, nullptr, 16); // hex
}

void parseArg(std::string& lh, const std::string& rh){
    lh = rh;
}

void PluginInfo::deserialize(std::istream& file) {
    // first check for sections, then for keys!
    bool start = false;
    std::string line;
    while (getLine(file, line)){
        if (line == "[plugin]"){
            start = true;
        } else if (line == "[parameters]"){
            parameters.clear();
            std::getline(file, line);
            int n = getCount(line);
            while (n-- && std::getline(file, line)){
                auto args = splitString(line, ',');
                Param param;
                if (args.size() >= 2){
                    param.name = rtrim(args[0]);
                    param.label = ltrim(args[1]);
                }
                if (args.size() >= 3){
                    param.id = std::stol(args[2], nullptr, 16); // hex
                }
                parameters.push_back(std::move(param));
            }
            // inverse mapping name -> index
            for (int i = 0; i < (int)parameters.size(); ++i){
                auto& param = parameters[i];
                paramMap_[param.name] = i;
            #if USE_VST3
                // for VST3:
                idToIndexMap_[param.id] = i;
                indexToIdMap_[i] = param.id;
            #endif
            }
        } else if (line == "[programs]"){
            programs.clear();
            std::getline(file, line);
            int n = getCount(line);
            while (n-- && std::getline(file, line)){
                programs.push_back(std::move(line));
            }
            // finished if we're not a shell plugin (a bit hacky...)
            if (category != "Shell"){
                break; // done!
            }
#if USE_VST2
        } else if (line == "[shell]"){
            shellPlugins.clear();
            std::getline(file, line);
            int n = getCount(line);
            while (n-- && std::getline(file, line)){
                auto pos = line.find(',');
                ShellPlugin shell;
                shell.name = rtrim(line.substr(0, pos));
                shell.id = std::stol(line.substr(pos + 1));
                shellPlugins.push_back(std::move(shell));
            }
            break; // done!
#endif
        } else if (start){
            std::string key;
            std::string value;
            getKeyValuePair(line, key, value);
            #define MATCH(name, field) else if (name == key) parseArg(field, value)
            try {
                if (key == "id"){
                    if (value.size() == 8){
                        type_ == PluginType::VST2;
                        sscanf(&value[0], "%08X", &id_.id);
                    } else if (value.size() == 32){
                        type_ == PluginType::VST3;
                        const int n = value.size() / 2;
                        for (int i = 0; i < n; ++i){
                            char buf[3] = { 0 };
                            memcpy(buf, &value[i * 2], 2);
                            unsigned int temp;
                            sscanf(buf, "%02x", &temp);
                            id_.uid[i] = temp;
                        }
                    } else {
                        throw Error("bad id!");
                    }
                    uniqueID = value;
                }
                MATCH("path", path);
                MATCH("name", name);
                MATCH("vendor", vendor);
                MATCH("category", category);
                MATCH("version", version);
                MATCH("sdkversion", sdkVersion);
                MATCH("inputs", numInputs);
                MATCH("auxinputs", numAuxInputs);
                MATCH("outputs", numOutputs);
                MATCH("auxoutputs", numAuxOutputs);
            #if USE_VST3
                MATCH("pgmchange", programChange);
                MATCH("bypass", bypass);
            #endif
                MATCH("flags", flags); // hex
                else {
                    LOG_WARNING("unknown key: " << key);
                    // throw Error("unknown key: " + key);
                }
            }
            #undef MATCH
            catch (const std::invalid_argument& e) {
                throw Error("invalid argument for key '" + key + "': " + value);
            }
            catch (const std::out_of_range& e) {
                throw Error("out of range argument for key '" + key + "': " + value);
            }
            catch (const std::exception& e){
                throw Error("unknown error: " + std::string(e.what()));
            }
        } else {
            throw Error("bad data: " + line);
        }
    }
}

} // vst


