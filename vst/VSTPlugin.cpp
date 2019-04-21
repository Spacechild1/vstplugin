#include "VSTPluginInterface.h"
#include "Utility.h"
#if USE_VST2
#include "VST2Plugin.h"
#endif
#if USE_VST3
#include "VST3Plugin.h"
#endif

#include <unordered_set>
#include <stdlib.h>
#include <cstring>
#include <sstream>

#ifdef _WIN32
# include <Windows.h>
# include <process.h>
#include <io.h>
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

std::string expandPath(const char *path) {
    char buf[MAX_PATH];
    ExpandEnvironmentStringsA(path, buf, MAX_PATH);
    return buf;
}
#else
#define widen(x) x
#define shorten(x) x

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

#if _WIN32
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

#ifndef _WIN32
// helper function
static bool isDirectory(dirent *entry){
    // we don't count "." and ".."
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0){
        return false;
    }
#ifdef _DIRENT_HAVE_D_TYPE
    // some filesystems don't support d_type, also we want to follow symlinks
    if (entry->d_type != DT_UNKNOWN && d_type != DT_LNK){
        return (entry->d_type == DT_DIR);
    } else
#endif
    {
        struct stat stbuf;
        if (stat(entry->d_name, &stbuf) == 0){
            return S_ISDIR(stbuf.st_mode);
        }
        return false;
    }
}
#endif

// recursively search for a VST plugin in a directory. returns empty string on failure.
std::string search(const std::string &dir, const std::string &path){
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
                auto extPos = name.find('.');
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

/*////////////////////////// probe ///////////////////////////*/

// probe a plugin in a seperate process and return the info in a file
ProbeResult probe(const std::string& path, const std::string& name, VSTPluginDesc& desc) {
	int result = -1;
#ifdef _WIN32
	// create temp file path (tempnam works differently on MSVC and MinGW, so we use the Win32 API instead)
	wchar_t tmpDir[MAX_PATH + 1];
	auto tmpDirLen = GetTempPathW(MAX_PATH, tmpDir);
	_snwprintf(tmpDir + tmpDirLen, MAX_PATH - tmpDirLen, L"vst%x", (uint32_t)rand()); // lazy
	std::wstring tmpPath = tmpDir;
    /// LOG_DEBUG("temp path: " << shorten(tmpPath));
	// get full path to probe exe
	std::wstring probePath = getDirectory() + L"\\probe.exe";
    /// LOG_DEBUG("probe path: " << shorten(probePath));
	// on Windows we need to quote the arguments for _spawn to handle spaces in file names.
	std::wstring quotedPluginPath = L"\"" + widen(path) + L"\"";
    std::wstring quotedPluginName = L"\"" + widen(name) + L"\"";
	std::wstring quotedTmpPath = L"\"" + tmpPath + L"\"";
	// start a new process with plugin path and temp file path as arguments:
    result = _wspawnl(_P_WAIT, probePath.c_str(), L"probe.exe", quotedPluginPath.c_str(), quotedPluginName.c_str(), quotedTmpPath.c_str(), nullptr);
#else // Unix
	// create temp file path
	auto tmpBuf = tempnam(nullptr, nullptr);
	std::string tmpPath;
	if (tmpBuf) {
		tmpPath = tmpBuf;
		free(tmpBuf);
	}
	else {
		LOG_ERROR("couldn't make create file name");
        return ProbeResult::error;
	}
	/// LOG_DEBUG("temp path: " << tmpPath);
	Dl_info dlinfo;
	if (!dladdr((void *)probe, &dlinfo)) {
		LOG_ERROR("probePlugin: couldn't get module path!");
        return ProbeResult::error;
	}
	// get full path to probe exe
	std::string modulePath = dlinfo.dli_fname;
	auto end = modulePath.find_last_of('/');
	std::string probePath = modulePath.substr(0, end) + "/probe";
	// fork
	pid_t pid = fork();
	if (pid == -1) {
		LOG_ERROR("probePlugin: fork failed!");
        return ProbeResult::error;
	}
	else if (pid == 0) {
		// child process: start new process with plugin path and temp file path as arguments.
		// we must not quote arguments to exec!
		if (execl(probePath.c_str(), "probe", path.c_str(), name.c_str(), tmpPath.c_str(), nullptr) < 0) {
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
#ifdef _WIN32
		// there's no way to open a fstream with a wide character path...
		// (the C++17 standard allows filesystem::path but this isn't widely available yet)
		// for now let's assume temp paths are always ASCII. LATER fix this!
		std::ifstream file(shorten(tmpPath), std::ios::binary);
#else
		std::ifstream file(tmpPath, std::ios::binary);
#endif
		if (file.is_open()) {
			desc.deserialize(file);
            desc.path = path;
			file.close();
#ifdef _WIN32
			if (!fs::remove(tmpPath)) {
#else
			if (std::remove(tmpPath.c_str()) != 0) {
#endif
				LOG_ERROR("probePlugin: couldn't remove temp file!");
                return ProbeResult::error;
			}
            return ProbeResult::success;
        }
		else {
			LOG_ERROR("probePlugin: couldn't read temp file!");
            return ProbeResult::error;
		}
    }
	else if (result == EXIT_FAILURE) {
        return ProbeResult::fail;
	}
	else {
        return ProbeResult::crash;
    }
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

/*///////////////////// VSTPluginDesc /////////////////////*/

VSTPluginDesc::VSTPluginDesc(IVSTFactory &factory)
    : factory_(&factory){}

VSTPluginDesc::VSTPluginDesc(IVSTFactory& factory, IVSTPlugin& plugin)
    : VSTPluginDesc(factory)
{
	name = plugin.getPluginName();
    vendor = plugin.getPluginVendor();
    category = plugin.getPluginCategory();
    version = plugin.getPluginVersion();
	id = plugin.getPluginUniqueID();
	numInputs = plugin.getNumInputs();
	numOutputs = plugin.getNumOutputs();
	int numParameters = plugin.getNumParameters();
	parameters.clear();
	for (int i = 0; i < numParameters; ++i) {
		parameters.emplace_back(plugin.getParameterName(i), plugin.getParameterLabel(i));
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

void VSTPluginDesc::serialize(std::ofstream& file, char sep) const {
	file << path << sep;
	file << name << sep;
    file << vendor << sep;
    file << category << sep;
	file << version << sep;
	file << id << sep;
	file << numInputs << sep;
	file << numOutputs << sep;
	file << (int)parameters.size() << sep;
	file << (int)programs.size() << sep;
	file << (uint32_t)flags << sep;
	for (auto& param : parameters) {
		file << param.first << sep;
		file << param.second << sep;
	}
	for (auto& pgm : programs) {
		file << pgm << sep;
	}
}

void VSTPluginDesc::deserialize(std::ifstream& file, char sep) {
	try {
		std::vector<std::string> lines;
		std::string line;
		int numParameters = 0;
		int numPrograms = 0;
        int numLines = 11;
		while (numLines--) {
			std::getline(file, line, sep);
			lines.push_back(std::move(line));
		}
		path = lines[0];
		name = lines[1];
        vendor = lines[2];
        category = lines[3];
        version = lines[4];
        id = stol(lines[5]);
        numInputs = stol(lines[6]);
        numOutputs = stol(lines[7]);
        numParameters = stol(lines[8]);
        numPrograms = stol(lines[9]);
        flags = stoul(lines[10]);
		// parameters
		parameters.clear();
		for (int i = 0; i < numParameters; ++i) {
			std::pair<std::string, std::string> param;
			std::getline(file, param.first, sep);
			std::getline(file, param.second, sep);
			parameters.push_back(std::move(param));
		}
		// programs
		programs.clear();
		for (int i = 0; i < numPrograms; ++i) {
			std::string program;
			std::getline(file, program, sep);
			programs.push_back(std::move(program));
		}
	}
	catch (const std::invalid_argument& e) {
        LOG_ERROR("VSTPluginInfo::deserialize: invalid argument");
	}
	catch (const std::out_of_range& e) {
        LOG_ERROR("VSTPluginInfo::deserialize: out of range");
	}
}

bool VSTPluginDesc::unique() const {
    return (factory_ ? factory_->numPlugins() == 1 : false);
}

std::unique_ptr<IVSTPlugin> VSTPluginDesc::create() const {
    return factory_ ? factory_->create(name) : nullptr;
}

} // vst


