#include "VSTPluginInterface.h"
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
    char buf[MAX_PATH];
    ExpandEnvironmentStringsA(path, buf, MAX_PATH);
    return buf;
}
#else
std::string expandPath(const char *path) {
    // expand ~ to home directory
    if (path && *path == '~') {
        const char *home = getenv("HOME");
        if (home) {
            return std::string(home) + std::string(path + 1);
        }
    }
    return path;
}
#endif

/*////////////////////// search ///////////////////////*/

static std::vector<const char *> platformExtensions = {
#ifdef __APPLE__
    ".vst",
#endif
#ifdef _WIN32
    ".dll",
#endif
#ifdef __linux__
    ".so",
#endif
    ".vst3"
};

const std::vector<const char *>& getPluginExtensions() {
    return platformExtensions;
}

static std::vector<const char *> defaultSearchPaths = {
	// macOS
#ifdef __APPLE__
	"~/Library/Audio/Plug-Ins/VST", "/Library/Audio/Plug-Ins/VST"
#endif
	// Windows
#ifdef _WIN32
#ifdef _WIN64 // 64 bit
#define PROGRAMFILES "%ProgramFiles%\\"
#else // 32 bit
#define PROGRAMFILES "%ProgramFiles(x86)%\\"
#endif
    PROGRAMFILES "VSTPlugins", PROGRAMFILES "Steinberg\\VSTPlugins",
    PROGRAMFILES "Common Files\\VST2", PROGRAMFILES "Common Files\\Steinberg\\VST2"
#undef PROGRAMFILES
#endif
	// Linux
#ifdef __linux__
    "/usr/local/lib/vst", "/usr/lib/vst"
#endif
};

// expanded search paths
static std::vector<std::string> realDefaultSearchPaths;

const std::vector<std::string>& getDefaultSearchPaths() {
    // not thread safe (yet)
    if (realDefaultSearchPaths.empty()) {
        for (auto& path : defaultSearchPaths) {
            realDefaultSearchPaths.push_back(expandPath(path));
        }
    }
    return realDefaultSearchPaths;
}

bool fileExists(const std::string& path){
#ifdef _WIN32
    return fs::exists(widen(path));
#else
    struct stat stbuf;
    return stat(fname.c_str(), &stbuf) == 0;
#endif
}

#ifndef _WIN32
// helper function
static bool isDirectory(dirent *entry){
    // we don't count "." and ".."
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0){
        return false;
    }
#ifdef _DIRENT_HAVE_D_TYPE
    // some filesystems don't support d_type, also we want to follow symlinks
    if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_LNK){
        return (entry->d_type == DT_DIR);
    } else
#endif
    {
        struct stat stbuf;
        if (stat(entry->d_name, &stbuf) == 0){
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
        auto fpath = fs::path(relpath);
        auto file = fs::path(dir) / fpath;
        if (fs::is_regular_file(file)){
            return file.u8string(); // success
        }
        // continue recursively
        for (auto& entry : fs::recursive_directory_iterator(dir)) {
            if (fs::is_directory(entry)){
                file = entry.path() / fpath;
                if (fs::is_regular_file(file)){
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

    auto isFile = [](const std::string& fname){
        struct stat stbuf;
        return stat(fname.c_str(), &stbuf) == 0;
    };

    std::string file = root + "/" + relpath;
    if (isFile(file)){
        return file; // success
    }
    // continue recursively
    std::function<void(const std::string&)> searchDir = [&](const std::string& dirname) {
        DIR *directory = opendir(dirname.c_str());
        struct dirent *entry;
        if (directory){
            while (result.empty() && (entry = readdir(directory))){
                if (isDirectory(entry)){
                    std::string d = dirname + "/" + entry->d_name;
                    std::string absPath = d + "/" + relpath;
                    if (isFile(absPath)){
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
    try {
        for (auto& entry : fs::recursive_directory_iterator(dir)) {
            if (fs::is_regular_file(entry)) {
                auto ext = entry.path().extension().u8string();
                if (extensions.count(ext)) {
                    auto abspath = entry.path().u8string();
                    auto basename = entry.path().filename().u8string();
                    fn(abspath, basename);
                }
            }
        }
    } catch (const fs::filesystem_error& e) {};
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
                // *first* check the extensions because VST plugins can be files (Linux) or directories (macOS)
                std::string ext;
                auto extPos = name.find_last_of('.');
                if (extPos != std::string::npos) {
                    ext = name.substr(extPos);
                }
                if (extensions.count(ext)) {
                    fn(absPath, name);
                }
                // otherwise search it if it's a directory
                else if (isDirectory(entry)) {
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

/*/////////// IVSTWindow //////////////////*/
namespace VSTWindowFactory {
#ifdef _WIN32
    void initializeWin32();
    IVSTWindow *createWin32(IVSTPlugin &);
#endif
#ifdef __APPLE__
    void initializeCocoa();
    IVSTWindow *createCocoa(IVSTPlugin &);
    void pollCocoa();
#endif
#ifdef USE_X11
    void initializeX11();
    IVSTWindow *createX11(IVSTPlugin &);
#endif
}

std::unique_ptr<IVSTWindow> IVSTWindow::create(IVSTPlugin &plugin){
    IVSTWindow *win;
#ifdef _WIN32
    win = VSTWindowFactory::createWin32(plugin);
#elif defined(__APPLE__)
    win = VSTWindowFactory::createCocoa(plugin);
#elif defined(USE_X11)
    win = VSTWindowFactory::createX11(plugin);
#endif
    return std::unique_ptr<IVSTWindow>(win);
}

void IVSTWindow::initialize(){
#ifdef _WIN32
    VSTWindowFactory::initializeWin32();
#endif
#ifdef __APPLE__
    VSTWindowFactory::initializeCocoa();
#endif
#ifdef USE_X11
    VSTWindowFactory::initializeX11();
#endif
}

void IVSTWindow::poll(){
#ifdef __APPLE__
    VSTWindowFactory::pollCocoa();
#endif
}

/*///////////// IModule //////////////*/

// from VST3_SDK/pluginterfaces/base/fplatform.h
#ifdef _WIN32
#define PLUGIN_API __stdcall
#else
#define PLUGIN_API
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
            ss << "LoadLibrary failed with error code " << error;
            throw VSTError(ss.str());
        }
        // LOG_DEBUG("loaded Win32 library " << path);
    }
    ~ModuleWin32(){
        FreeLibrary(handle_);
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
            throw VSTError("couldn't create bundle reference");
        }
        // LOG_DEBUG("loaded macOS bundle " << path);
    }
    ~ModuleApple(){
        CFRelease(bundle_);
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
            ss << "dlopen failed with error code " << (error ? error : "?");
            throw VSTError(ss.str());
        }
        // LOG_DEBUG("loaded dynamic library " << path);
    }
    ~ModuleSO(){
        dlclose(handle_);
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

/*///////////////////// IVSTFactory ////////////////////////*/

std::unique_ptr<IVSTFactory> IVSTFactory::load(const std::string& path){
#ifdef _WIN32
    const char *ext = ".dll";
#elif defined(__APPLE__)
	const char *ext = ".vst";
#else // Linux/BSD/etc.
    const char *ext = ".so";
#endif
    try {
        // LOG_DEBUG("IVSTFactory: loading " << path);
        if (path.find(".vst3") != std::string::npos){
        #if USE_VST3
            return std::make_unique<VST3Factory>(path);
        #else
            LOG_WARNING("VST3 plug-ins not supported!");
            return nullptr;
        #endif
        } else {
        #if USE_VST2
            if (path.find(ext) != std::string::npos){
                return std::make_unique<VST2Factory>(path);
            } else {
                return std::make_unique<VST2Factory>(path + ext);
            }
        #else
            LOG_WARNING("VST2.x plug-ins not supported!");
            return nullptr;
        #endif
        }
    } catch (const VSTError& e){
        LOG_ERROR("couldn't load '" << path << "':");
        LOG_ERROR(e.what());
        return nullptr;
    }
}

// RAII class for automatic cleanup
class TmpFile : public std::fstream {
public:
#ifdef _WIN32
    TmpFile(const std::wstring& path)
        : std::fstream(path.c_str(), std::ios::binary | std::ios::in | std::ios::out),
          path_(path){
        // if (!is_open()) throw (VSTError("couldn't open " + shorten(path)));
    }
#else
    TmpFile(const std::string& path)
        : std::fstream(path, std::ios::binary | std::ios::in | std::ios::out),
          path_(path){}
#endif
    ~TmpFile(){
        close();
        // destructor must not throw!
        try {
    #ifdef _WIN32
            fs::remove(path_);
    #else
            std::remove(path_.c_str());
    #endif
        } catch (const std::exception& e) { LOG_ERROR(e.what()); };
    }
private:
#ifdef _WIN32
    std::wstring path_;
#else
    std::string path_;
#endif
};

// probe a plugin in a seperate process and return the info in a file
VSTPluginDescPtr IVSTFactory::probePlugin(const std::string& name, int shellPluginID) {
    int result = -1;
    VSTPluginDesc desc(*this);
    // put the information we already have (might be overriden)
    desc.name = name;
    desc.id = shellPluginID;
    desc.path = path();
    // we pass the shell plugin ID instead of the name to probe.exe
    std::string pluginName = shellPluginID ? std::to_string(shellPluginID) : name;
#ifdef _WIN32
    // create temp file path (tempnam works differently on MSVC and MinGW, so we use the Win32 API instead)
    wchar_t tmpDir[MAX_PATH + 1];
    auto tmpDirLen = GetTempPathW(MAX_PATH, tmpDir);
    _snwprintf(tmpDir + tmpDirLen, MAX_PATH - tmpDirLen, L"vst_%x", (uint64_t)this); // lazy
    std::wstring tmpPath = tmpDir;
    /// LOG_DEBUG("temp path: " << shorten(tmpPath));
    // get full path to probe exe
    std::wstring probePath = getDirectory() + L"\\probe.exe";
    /// LOG_DEBUG("probe path: " << shorten(probePath));
    // on Windows we need to quote the arguments for _spawn to handle spaces in file names.
    std::wstring quotedPluginPath = L"\"" + widen(path()) + L"\"";
    std::wstring quotedPluginName = L"\"" + widen(pluginName) + L"\"";
    std::wstring quotedTmpPath = L"\"" + tmpPath + L"\"";
    // start a new process with plugin path and temp file path as arguments:
    result = _wspawnl(_P_WAIT, probePath.c_str(), L"probe.exe", quotedPluginPath.c_str(),
                      quotedPluginName.c_str(), quotedTmpPath.c_str(), nullptr);
#else // Unix
    // create temp file path
    std::string tmpPath
#if 1
    auto tmpBuf = tempnam(nullptr, nullptr);
    if (tmpBuf) {
        tmpPath = tmpBuf;
        free(tmpBuf);
    }
    else {
        throw VSTError("couldn't create tmp file name");
    }
#else
    char tmpBuf[MAX_PATH + 1];
    const char *tmpDir = nullptr;
    auto tmpVarList = { "TMPDIR", "TMP", "TEMP", "TEMPDIR", nullptr };
    auto tmpVar = tmpVarList;
    while (*tmpVar++ && !tmpDir){
        tmpDir = getenv(*tmpVar);
    }
    snprintf(tmpBuf, MAX_PATH, L"%s/vst_%x", (tmpDir ? tmpDir : "/tmp"), (uint64_t)this); // lazy
    tmpPath = tmpBuf;
#endif
    /// LOG_DEBUG("temp path: " << tmpPath);
    Dl_info dlinfo;
    // get full path to probe exe
    // hack: obtain library info through a function pointer (vst::search)
    if (!dladdr((void *)search, &dlinfo)) {
        throw VSTError("probePlugin: couldn't get module path!");
    }
    std::string modulePath = dlinfo.dli_fname;
    auto end = modulePath.find_last_of('/');
    std::string probePath = modulePath.substr(0, end) + "/probe";
    // fork
    pid_t pid = fork();
    if (pid == -1) {
        throw VSTError("probePlugin: fork failed!");
    }
    else if (pid == 0) {
        // child process: start new process with plugin path and temp file path as arguments.
        // we must not quote arguments to exec!
        if (execl(probePath.c_str(), "probe", path().c_str(), pluginName.c_str(), tmpPath.c_str(), nullptr) < 0) {
            LOG_ERROR("probePlugin: exec failed!");
        }
        std::exit(EXIT_FAILURE);
    }
    else {
        // parent process: wait for child
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            result = WEXITSTATUS(status);
        }
    }
#endif
    /// LOG_DEBUG("result: " << result);
    if (result == EXIT_SUCCESS) {
        // get info from temp file
        TmpFile file(tmpPath);
        if (file.is_open()) {
            desc.deserialize(file);
            desc.probeResult = ProbeResult::success;
        }
        else {
            throw VSTError("probePlugin: couldn't read temp file!");
        }
    }
    else if (result == EXIT_FAILURE) {
        desc.probeResult = ProbeResult::fail;
    }
    else {
        desc.probeResult = ProbeResult::crash;
    }
    return std::make_shared<VSTPluginDesc>(std::move(desc));
}

/*///////////////////// VSTPluginDesc /////////////////////*/

VSTPluginDesc::VSTPluginDesc(const IVSTFactory &factory)
    : path(factory.path()), factory_(&factory) {}

VSTPluginDesc::VSTPluginDesc(const IVSTFactory& factory, const IVSTPlugin& plugin)
    : VSTPluginDesc(factory)
{
    name = plugin.getPluginName();
    if (name.empty()){
        auto sep = path.find_last_of("\\/");
        auto dot = path.find_last_of('.');
        if (sep == std::string::npos){
            sep = -1;
        }
        if (dot == std::string::npos){
            dot = path.size();
        }
        name = path.substr(sep + 1, dot - sep - 1);
    }
    vendor = plugin.getPluginVendor();
    category = plugin.getPluginCategory();
    version = plugin.getPluginVersion();
	id = plugin.getPluginUniqueID();
	numInputs = plugin.getNumInputs();
	numOutputs = plugin.getNumOutputs();
	int numParameters = plugin.getNumParameters();
	parameters.clear();
	for (int i = 0; i < numParameters; ++i) {
        Param param{plugin.getParameterName(i), plugin.getParameterLabel(i)};
        parameters.push_back(std::move(param));
	}
    // inverse mapping from name to index
    for (int i = 0; i < numParameters; ++i){
        paramMap[parameters[i].name] = i;
    }
	int numPrograms = plugin.getNumPrograms();
	programs.clear();
	for (int i = 0; i < numPrograms; ++i) {
		auto pgm = plugin.getProgramNameIndexed(i);
#if 0
		if (pgm.empty()) {
			plugin.setProgram(i);
			pgm = plugin.getProgramName();
		}
#endif
		programs.push_back(pgm);
	}
	flags = 0;
	flags |= plugin.hasEditor() << HasEditor;
	flags |= plugin.isSynth() << IsSynth;
	flags |= plugin.hasPrecision(VSTProcessPrecision::Single) << SinglePrecision;
	flags |= plugin.hasPrecision(VSTProcessPrecision::Double) << DoublePrecision;
	flags |= plugin.hasMidiInput() << MidiInput;
	flags |= plugin.hasMidiOutput() << MidiOutput;
}

/// .ini file structure for each plugin:
///
/// [plugin]
/// path=<string>
/// name=<string>
/// vendor=<string>
/// category=<string>
/// version=<string>
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


void VSTPluginDesc::serialize(std::ostream& file) const {
    file << "[plugin]\n";
    file << "path=" << path << "\n";
    file << "name=" << name << "\n";
    file << "vendor=" << vendor << "\n";
    file << "category=" << category << "\n";
    file << "version=" << version << "\n";
    file << "id=" << id << "\n";
    file << "inputs=" << numInputs << "\n";
    file << "outputs=" << numOutputs << "\n";
    file << "flags=" << (uint32_t)flags << "\n";
    // parameters
    file << "[parameters]\n";
    file << "n=" << parameters.size() << "\n";
    for (auto& param : parameters) {
        file << bashString(param.name) << "," << param.label << "\n";
	}
    // programs
    file << "[programs]\n";
    file << "n=" << programs.size() << "\n";
	for (auto& pgm : programs) {
        file << pgm << "\n";
	}
    // shell plugins (only used for probe.exe)
    if (!shellPlugins_.empty()){
        file << "[shell]\n";
        file << "n=" << (int)shellPlugins_.size() << "\n";
        for (auto& shell : shellPlugins_){
            file << shell.name << "," << shell.id << "\n";
        }
    }
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

static bool isComment(const std::string& line){
    auto c = line.front();
    return c == ';' || c == '#';
}

static int getCount(const std::string& line){
    LOG_DEBUG(line);
    auto pos = line.find('=');
    if (pos == std::string::npos){
        throw VSTError("VSTPluginDesc::serialize: "
                       "missing '=' after key: " + line);
    }
    return std::stol(line.substr(pos + 1));
}

static void getKeyValuePair(const std::string& line, std::string& key, std::string& value){
    auto pos = line.find('=');
    if (pos == std::string::npos){
        throw VSTError("VSTPluginDesc::serialize: "
                       "missing '=' after key: " + line);
    }
    key = rtrim(line.substr(0, pos));
    value = ltrim(line.substr(pos + 1));
}

void VSTPluginDesc::deserialize(std::istream& file) {
    // first check for sections, then for keys!
    bool start = false;
    std::string line;
    try {
        while (std::getline(file, line)){
            LOG_DEBUG(line);
            if (isComment(line)){
                continue;
            } else if (line == "[plugin]"){
                start = true;
            } else if (line == "[parameters]"){
                parameters.clear();
                std::getline(file, line);
                int n = getCount(line);
                while (n-- && std::getline(file, line)){
                    auto pos = line.find(',');
                    Param param;
                    param.name = rtrim(line.substr(0, pos));
                    param.label = ltrim(line.substr(pos + 1));
                    parameters.push_back(std::move(param));
                }
                // inverse mapping from name to index
                for (int i = 0; i < (int)parameters.size(); ++i){
                    paramMap[parameters[i].name] = i;
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
            } else if (line == "[shell]"){
                shellPlugins_.clear();
                std::getline(file, line);
                int n = getCount(line);
                while (n-- && std::getline(file, line)){
                    auto pos = line.find(',');
                    ShellPlugin shell;
                    shell.name = rtrim(line.substr(0, pos));
                    shell.id = std::stol(line.substr(pos + 1));
                    shellPlugins_.push_back(std::move(shell));
                }
                break; // done!
            } else if (start){
                std::string key;
                std::string value;
                getKeyValuePair(line, key, value);
                if (key == "path"){
                    path = std::move(value);
                } else if (key == "name"){
                    name = std::move(value);
                } else if (key == "vendor"){
                    vendor = std::move(value);
                } else if (key == "category"){
                    category = std::move(value);
                } else if (key == "version"){
                    version = std::move(value);
                } else if (key == "id"){
                    id = std::stol(value);
                } else if (key == "inputs"){
                    numInputs = std::stol(value);
                } else if (key == "outputs"){
                    numOutputs = std::stol(value);
                } else if (key == "flags"){
                    flags = std::stol(value);
                } else {
                    throw VSTError("VSTPluginDesc::serialize: unknown key: " + key);
                }
            } else {
                throw VSTError("VSTPluginDesc::serialize: bad data: " + line);
            }
        }
	}
	catch (const std::invalid_argument& e) {
        throw VSTError("VSTPluginInfo::deserialize: invalid argument: " + line);
	}
	catch (const std::out_of_range& e) {
        throw VSTError("VSTPluginInfo::deserialize: out of range: " + line);
	}
}

std::unique_ptr<IVSTPlugin> VSTPluginDesc::create() const {
    return factory_ ? factory_->create(name) : nullptr;
}

} // vst


