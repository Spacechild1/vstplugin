#include "VST2Plugin.h"
#include "Utility.h"

#include <unordered_set>

#ifdef _WIN32
# include <Windows.h>
# include <process.h>
#include <io.h>
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else // just because of Clang on macOS not shipping <experimental/filesystem>...
# include <dirent.h>
# include <unistd.h>
# include <strings.h>
# include <sys/wait.h>
#endif

#if DL_OPEN
# include <dlfcn.h>
# include <stdlib.h>
#endif

#ifdef __APPLE__
# include <CoreFoundation/CoreFoundation.h>
# include <mach-o/dyld.h>
# include <unistd.h>
#endif

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
#else
#define widen(x) x
#define shorten(x) x
#endif

IVSTPlugin* loadVSTPlugin(const std::string& path, bool silent){
    AEffect *plugin = nullptr;
    vstPluginFuncPtr mainEntryPoint = nullptr;
    bool openedlib = false;
	std::string fullPath = path;
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
	if (dot == std::string::npos || path.find(ext, dot) == std::string::npos) {
		fullPath += ext;
	}
#ifdef _WIN32
    if(!mainEntryPoint) {
        HMODULE handle = LoadLibraryW(widen(fullPath).c_str());
        if (handle) {
            openedlib = true;
            mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "VSTPluginMain"));
            if (!mainEntryPoint){
                mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "main"));
            }
            if (!mainEntryPoint){
                FreeLibrary(handle);
            }
        } else if (!silent) {
            LOG_ERROR("loadVSTPlugin: LoadLibrary failed for " << fullPath);
        }
    }
#endif
#if defined __APPLE__
    if(!mainEntryPoint) {
            // Create a fullPath to the bundle
            // kudos to http://teragonaudio.com/article/How-to-make-your-own-VST-host.html
        CFStringRef pluginPathStringRef = CFStringCreateWithCString(NULL,
            fullPath.c_str(), kCFStringEncodingUTF8);
        CFURLRef bundleUrl = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
            pluginPathStringRef, kCFURLPOSIXPathStyle, true);
        CFBundleRef bundle = nullptr;
        if(bundleUrl) {
                // Open the bundle
            bundle = CFBundleCreate(kCFAllocatorDefault, bundleUrl);
            if(!bundle && !silent) {
                LOG_ERROR("loadVSTPlugin: couldn't create bundle reference for " << fullPath);
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
        void *handle = dlopen(fullPath.c_str(), RTLD_NOW | RTLD_DEEPBIND);
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
        } else if (!silent) {
            LOG_ERROR("loadVSTPlugin: couldn't dlopen " << fullPath);
        }
    }
#endif

    if (!openedlib) // we already printed an error if finding/opening the plugin filed
        return nullptr;

    if (!mainEntryPoint && !silent){
        LOG_ERROR("loadVSTPlugin: couldn't find entry point in VST plugin");
        return nullptr;
    }
    plugin = mainEntryPoint(&VST2Plugin::hostCallback);
    if (!plugin && !silent){
        LOG_ERROR("loadVSTPlugin: couldn't initialize plugin");
        return nullptr;
    }
    if (plugin->magic != kEffectMagic && !silent){
        LOG_ERROR("loadVSTPlugin: not a VST plugin!");
        return nullptr;
    }
    return new VST2Plugin(plugin, fullPath);
}

void freeVSTPlugin(IVSTPlugin *plugin){
    if (plugin){
        delete plugin;
    }
}

namespace VSTWindowFactory {
        // forward declarations
#ifdef _WIN32
    IVSTWindow * createWin32(IVSTPlugin *plugin);
    void initializeWin32();
#endif
#if USE_X11
    void initializeX11();
    IVSTWindow * createX11(IVSTPlugin *plugin);
#endif
#ifdef __APPLE__
    void initializeCocoa();
    IVSTWindow * createCocoa(IVSTPlugin *plugin);
    void mainLoopPollCocoa();
#endif
        // initialize
    void initialize(){
#ifdef _WIN32
        initializeWin32();
#endif
#if USE_X11
        initializeX11();
#endif
#ifdef __APPLE__
        initializeCocoa();
#endif
    }
        // create
    IVSTWindow* create(IVSTPlugin *plugin){
        IVSTWindow *win = nullptr;
#ifdef _WIN32
        win = createWin32(plugin);
#elif defined(__APPLE__)
        win = createCocoa(plugin);
#elif USE_X11
        win = createX11(plugin);
#endif
        return win;
    }
        // poll
#ifdef __APPLE__
    void mainLoopPoll(){
        mainLoopPollCocoa();
    }
#else
    void mainLoopPoll(){}
#endif
}

#ifdef _WIN32
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

const std::vector<std::string>& getDefaultSearchPaths() {
	// not thread safe (yet)
	if (realDefaultSearchPaths.empty()) {
		for (auto& path : defaultSearchPaths) {
			realDefaultSearchPaths.push_back(expandPath(path));
		}
	}
	return realDefaultSearchPaths;
}

const std::vector<const char *>& getPluginExtensions() {
	return platformExtensions;
}

void VSTPluginInfo::set(IVSTPlugin& plugin) {
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

void VSTPluginInfo::serialize(std::ofstream& file, char sep) {
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

void VSTPluginInfo::deserialize(std::ifstream& file, char sep) {
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

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved){
    if (fdwReason == DLL_PROCESS_ATTACH){
        hInstance = hinstDLL;
    }
    return true;
}
#endif

#ifdef _WIN32
#define CHAR wchar_t
#else
#define CHAR char
#endif
// probe a plugin and write info to file.
// used in probePlugin, but can be also exported and called by probe.exe
extern "C" {
    int probe(const CHAR *pluginPath, const CHAR *filePath){
        auto plugin = loadVSTPlugin(shorten(pluginPath), true);
        if (plugin) {
            if (filePath){
                VSTPluginInfo info;
                info.set(*plugin);
                // there's no way to open a fstream with a wide character path...
                // (the C++17 standard allows filesystem::path but this isn't widely available yet)
                // for now let's assume temp paths are always ASCII. LATER fix this!
                std::ofstream file(shorten(filePath), std::ios::binary);
                if (file.is_open()) {
                    info.serialize(file);
                }
            }
            freeVSTPlugin(plugin);
            return 1;
        }
        return 0;
    }
}
#undef CHAR

#ifdef _WIN32
#define DUP _dup
#define DUP2 _dup2
#define FILENO _fileno
#define CLOSE _close
#else
#define DUP dup
#define DUP2 dup2
#define FILENO fileno
#define CLOSE close
#endif
// scan for plugins on the Server
VstProbeResult probePlugin(const std::string& path, VSTPluginInfo& info) {
	int result = 0;
#ifdef _WIN32
    // tmpnam/tempnam work differently on MSVC and MinGW, so we use the Win32 API instead
    wchar_t tmpDir[MAX_PATH+1];
    auto tmpDirLen = GetTempPathW(MAX_PATH, tmpDir);
    _snwprintf(tmpDir + tmpDirLen, MAX_PATH - tmpDirLen, L"%p", (void *)tmpDir);
    std::wstring tmpPath = tmpDir;
    // LOG_DEBUG("temp path: " << shorten(tmpPath));
#else
    std::string tmpPath = tmpnam(nullptr);
    // LOG_DEBUG("temp path: " << tmpPath);
#endif
	// temporarily disable stdout
#if 1
	fflush(stdout);
	int oldStdOut = DUP(FILENO(stdout));
	FILE *nullOut = fopen("NUL", "w");
	DUP2(FILENO(nullOut), FILENO(stdout));
#endif
#ifdef _WIN32
    // probe the plugin in a new process
    std::wstring probePath = getDirectory() + L"\\probe.exe";
    std::wstring quotedPluginPath = L"\"" + widen(path) + L"\"";
    std::wstring quotedTmpPath = L"\"" + tmpPath + L"\"";
    // start new process with plugin path and temp file path as arguments
    result = _wspawnl(_P_WAIT, probePath.c_str(), L"probe.exe", quotedPluginPath.c_str(), quotedTmpPath.c_str(), nullptr);
#else // Unix
	// fork
	pid_t pid = fork();
	if (pid == -1) {
		LOG_ERROR("probePluging: fork failed!");
		return VstProbeResult::error;
	}
	else if (pid == 0) {
		// child process
        auto result = probe(path.c_str(), tmpPath.c_str());
        std::exit(result);
	}
	else {
		// parent process (waiting for child)
		waitpid(pid, &result, 0);
	}
#endif
#if 1
	// restore stdout
	fflush(stdout);
	fclose(nullOut);
	DUP2(oldStdOut, FILENO(stdout));
	CLOSE(oldStdOut);
#endif
	if (result > 0) {
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
			info.deserialize(file);
			info.path = path;
			file.close();
        #ifdef _WIN32
            if (!fs::remove(tmpPath)) {
        #else
            if (std::remove(tmpPath.c_str()) != 0) {
        #endif
				LOG_ERROR("probePlugin: couldn't remove temp file!");
				return VstProbeResult::error;
			}
			return VstProbeResult::success;
		}
		else {
			LOG_ERROR("probePlugin: couldn't read temp file!");
			return VstProbeResult::error;
		}
	}
	else if (result == 0) {
		return VstProbeResult::fail;
	}
	else {
		return VstProbeResult::crash;
	}
}
#undef DUP
#undef DUP2
#undef FILENO
#undef CLOSE

void searchPlugins(const std::string &dir, std::function<void(const std::string&, const std::string&)> fn) {
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
	// force trailing slash
	auto root = (dir.back() != '/') ? dir + "/" : dir;
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

