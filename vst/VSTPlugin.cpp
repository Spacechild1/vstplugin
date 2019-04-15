#include "VST2Plugin.h"
#include "Utility.h"

#include <unordered_set>
#include <stdlib.h>
#include <cstring>

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
	".vst"
#endif
#ifdef _WIN32
	".dll"
#endif
#ifdef __linux__
	".so"
#endif
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

void search(const std::string &dir, std::function<void(const std::string&, const std::string&)> fn) {
    // extensions
    std::unordered_set<std::string> extensions;
    for (auto& ext : platformExtensions) {
        extensions.insert(ext);
    }
    // search recursively
#ifdef _WIN32
    try {
        // root will have a trailing slash
        auto root = fs::path(dir).u8string();
        for (auto& entry : fs::recursive_directory_iterator(root)) {
            if (fs::is_regular_file(entry)) {
                auto ext = entry.path().extension().u8string();
                if (extensions.count(ext)) {
                    auto absPath = entry.path().u8string();
                    // relPath: fullPath minus root minus extension (without leading slash)
                    auto relPath = absPath.substr(root.size() + 1, absPath.size() - root.size() - ext.size() - 1);
                    // only forward slashes
                    for (auto& c : relPath) {
                        if (c == '\\') c = '/';
                    }
                    fn(absPath, relPath);
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
                    // shortPath: fullPath minus root minus extension (without leading slash)
                    auto relPath = absPath.substr(root.size() + 1, absPath.size() - root.size() - ext.size() - 1);
                    fn(absPath, relPath);
                }
                // otherwise search it if it's a directory
                else if (entry->d_type == DT_DIR) {
                    if (strcmp(name.c_str(), ".") != 0 && strcmp(name.c_str(), "..") != 0) {
                        searchDir(absPath);
                    }
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

#ifdef _WIN32
class ModuleWin32 : public IModule {
 public:
    ModuleWin32(const std::string& path){
        handle_ = LoadLibraryW(widen(path).c_str());
        if (!handle_){
            auto error = GetLastError();
            LOG_ERROR("LoadLibrary failed for " << path << " with error code " << error);
            throw std::exception();
        }
        // LOG_DEBUG("loaded Win32 library " << path);
    }
    ~ModuleWin32(){
        FreeLibrary(handle_);
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
            LOG_ERROR("couldn't create bundle reference for " << path);
            throw std::exception();
        }
        // LOG_DEBUG("loaded macOS bundle " << path);
    }
    ~ModuleApple(){
        CFRelease(bundle_);
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
class ModuleDL : public IModule {
 public:
    ModuleDL(const std::string& path){
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_DEEPBIND);
        auto error = dlerror();
        if (!handle_) {
            LOG_ERROR("couldn't dlopen " << path << ": " << (error ? error : ""));
            throw std::exception();
        }
        // LOG_DEBUG("loaded dynamic library " << path);
    }
    ~ModuleDL(){
        dlclose(handle_);
    }
    void * doGetFnPtr(const char *name) const override {
        return dlsym(handle_, name);
    }
 private:
    void *handle_;
};
#endif


std::unique_ptr<IModule> IModule::load(const std::string& path){
    std::unique_ptr<IModule> module;
#ifdef _WIN32
    try {
        if (!module) module = std::make_unique<ModuleWin32>(path);
    } catch (const std::exception& e){}
#endif
#if defined __APPLE__
    try {
        if (!module) module = std::make_unique<ModuleApple>(path);
    } catch (const std::exception& e){}
#endif
#if DL_OPEN
    try {
        if (!module) module = std::make_unique<ModuleDL>(path);
    } catch (const std::exception& e){}
#endif
    return module;
}

/*///////////////////// IVSTFactory ////////////////////////*/

std::unique_ptr<IVSTFactory> IVSTFactory::load(const std::string& path){
	auto dot = path.find_last_of('.');
#ifdef _WIN32
	const char *ext = ".dll";
#elif defined(__linux__)
	const char *ext = ".so";
#elif defined(__APPLE__)
	const char *ext = ".vst";
#else
	LOG_ERROR("makeVSTPluginFilePath: unknown platform!");
	return name;
#endif
    try {
        // LOG_DEBUG("IVSTFactory: loading " << path);
        if (path.find(".vst3", dot) != std::string::npos){
            // return new VST3Factory(path);
            return nullptr;
        } else if (path.find(ext, dot) != std::string::npos){
            return std::make_unique<VST2Factory>(path);
        } else {
            return std::make_unique<VST2Factory>(path + ext);
        }
    } catch (const std::exception& e){
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

std::unique_ptr<IVSTPlugin> VSTPluginDesc::create() const {
    return factory_ ? factory_->create(name) : nullptr;
}

} // vst


