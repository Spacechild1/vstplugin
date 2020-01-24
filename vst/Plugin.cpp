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
#include <algorithm>

#ifdef _WIN32
# include <windows.h>
# include <process.h>
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
// IMAGE_FILE_MACHINE_ARM64 is only defined on Windows 8.1 and above
#ifndef IMAGE_FILE_MACHINE_ARM64
 #define IMAGE_FILE_MACHINE_ARM64 0xaa64
#endif
#else
// just because of Clang on macOS not shipping <experimental/filesystem>...
# include <dirent.h>
# include <unistd.h>
# include <strings.h>
# include <sys/wait.h>
# include <sys/stat.h>
# include <sys/types.h>
# ifndef ACCESSPERMS
#  define ACCESSPERMS (S_IRWXU|S_IRWXG|S_IRWXO)
# endif
// for probing (and dlopen)
# include <dlfcn.h>
# include <stdio.h>
// for thread management
# include <pthread.h>
#endif

#ifdef __APPLE__
# include <CoreFoundation/CoreFoundation.h>
# include <mach-o/dyld.h>
# include <unistd.h>
# include <mach/machine.h>
# include <mach-o/loader.h>
# include <mach-o/fat.h>
#endif

#if defined(__linux__)
# include <elf.h>
#endif

namespace vst {

std::string getVersionString(){
    std::stringstream ss;
    ss << VERSION_MAJOR << "." << VERSION_MINOR;
    if (VERSION_BUGFIX > 0){
        ss << "." << VERSION_BUGFIX;
    }
    if (VERSION_PRERELEASE > 0){
        ss << "-pre" << VERSION_PRERELEASE;
    }
    return ss.str();
}

// forward declarations to avoid including the header files
// (creates troubles with Cocoa)
#ifdef _WIN32
namespace Win32 {
namespace UIThread {
    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
#if HAVE_UI_THREAD
    bool checkThread();
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
    bool checkThread();
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
    bool checkThread();
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

static std::wstring getModuleDirectory(){
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

bool isFile(const std::string& path){
#ifdef _WIN32
    return fs::is_regular_file(widen(path));
#else
    struct stat stbuf;
    return (stat(path.c_str(), &stbuf) == 0) && S_ISREG(stbuf.st_mode);
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
    int result = remove(path.c_str());
    if (result != 0){
        LOG_ERROR(errorMessage(result));
        return false;
    } else {
        return true;
    }
#endif
}

bool renameFile(const std::string& from, const std::string& to){
#ifdef _WIN32
    std::error_code e;
    fs::rename(widen(from), widen(to), e);
    if (e){
        LOG_ERROR(e.message());
        return false;
    } else {
        return true;
    }
#else
    int result = rename(from.c_str(), to.c_str());
    if (result != 0){
        LOG_ERROR(errorMessage(result));
        return false;
    } else {
        return true;
    }
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
    } else {
    #if 0
        LOG_ERROR(errorMessage(result));
    #endif
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

std::string fileExtension(const std::string& path){
    auto dot = path.find_last_of('.');
    if (dot != std::string::npos){
        return path.substr(dot + 1);
    } else {
        return "";
    }
}

std::string fileBaseName(const std::string& path){
#ifdef _WIN32
    auto pos = path.find_last_of("/\\");
#else
    auto pos = path.find_last_of('/');
#endif
    auto dot = path.find_last_of('.');
    size_t start = (pos != std::string::npos) ? pos + 1 : 0;
    size_t n = (dot != std::string::npos) ? dot - start : std::string::npos;
    return path.substr(start, n);
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

std::string errorMessage(int err){
    std::stringstream ss;
#ifdef _WIN32
    wchar_t buf[1000];
    buf[0] = 0;
    auto size = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err,
                                    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, 1000, NULL);
    // omit trailing \r\n
    if (size > 1 && buf[size-2] == '\r'){
        buf[size-2] = 0;
    }
    ss << shorten(buf);
#else
    ss << strerror(err);
#endif
    // include error number
    ss << " [" << err << "]";
    return ss.str();
}

enum class CpuArch {
    unknown,
    amd64,
    i386,
    arm,
    aarch64,
    ppc,
    ppc64
};

CpuArch getHostCpuArchitecture(){
#if defined(__i386__) || defined(_M_IX86)
    return CpuArch::i386;
#elif defined(__x86_64__) || defined(_M_X64)
    return CpuArch::amd64;
#elif defined(__arm__) || defined(_M_ARM)
    return CpuArch::arm;
#elif defined(__aarch64__)
    return CpuArch::aarch64;
#elif defined(__ppc__)
    return CpuArch::ppc;
#elif defined(__ppc64__)
    return CpuArch::ppc64;
#else
    return CpuArch::unknown;
#endif
}

const char * getCpuArchString(CpuArch arch){
    switch (arch){
    case CpuArch::i386:
        return "i386";
    case CpuArch::amd64:
        return "amd64";
    case CpuArch::arm:
        return "arm";
    case CpuArch::aarch64:
        return "aarch64";
    case CpuArch::ppc:
        return "ppc";
    case CpuArch::ppc64:
        return "ppc64";
    default:
        return "unknown";
    }
}

template<typename T>
static void swap_bytes(T& i){
    const auto n = sizeof(T);
    char a[n];
    char b[n];
    memcpy(a, &i, n);
    for (int i = 0, j = n-1; i < n; ++i, --j){
        b[i] = a[j];
    }
    memcpy(&i, b, n);
}

namespace detail {
// Check a file path or bundle for contained CPU architectures
// If 'path' is a file, we throw an exception if it is not a library,
// but if 'path' is a bundle (= directory), we ignore any non-library files
// in the 'Contents' subfolder (so the resulting list might be empty).
// However, we always throw exceptions when we encounter errors.
std::vector<CpuArch> getCpuArchitectures(const std::string& path, bool bundle){
    std::vector<CpuArch> results;
    if (isDirectory(path)){
    #ifdef _APPLE__
        const char *dir = "/Contents/MacOS"; // only has a single binary folder
    #else
        const char *dir = "/Contents";
    #endif
         // plugin bundle: iterate over Contents/* recursively
        vst::search(path + dir, [&](const std::string& resPath){
            auto res = getCpuArchitectures(resPath, true); // bundle!
            results.insert(results.end(), res.begin(), res.end());
        }, false); // don't filter by VST extensions (needed for MacOS bundles!)
    } else {
        vst::File file(path);
        if (file.is_open()){
        #if defined(_WIN32) // Windows
            // read PE header
            // note: we don't have to worry about byte order (always LE)
            const uint16_t dos_signature = 0x5A4D;
            const char pe_signature[] = { 'P', 'E', 0, 0 };
            const auto header_size = 24; // PE signature + COFF header
            char data[1024]; // should be large enough for DOS stub
            file.read(data, sizeof(data));
            int nbytes = file.gcount();
            // check DOS signature
            if (nbytes > sizeof(dos_signature) && !memcmp(data, &dos_signature, sizeof(dos_signature))){
                int32_t offset;
                // get the file offset to the PE signature
                memcpy(&offset, &data[0x3C], sizeof(offset));
                if (offset < (sizeof(data) - header_size)){
                    const char *header = data + offset;
                    if (!memcmp(header, pe_signature, sizeof(pe_signature))){
                        header += sizeof(pe_signature);
                        // get CPU architecture
                        uint16_t arch;
                        memcpy(&arch, &header[0], sizeof(arch));
                        switch (arch){
                        case IMAGE_FILE_MACHINE_AMD64:
                            results.push_back(CpuArch::amd64);
                            break;
                        case IMAGE_FILE_MACHINE_I386:
                            results.push_back(CpuArch::i386);
                            break;
                        case IMAGE_FILE_MACHINE_POWERPC:
                            results.push_back(CpuArch::ppc);
                            break;
                        case IMAGE_FILE_MACHINE_ARM:
                            results.push_back(CpuArch::arm);
                            break;
                        case IMAGE_FILE_MACHINE_ARM64:
                            results.push_back(CpuArch::aarch64);
                            break;
                        default:
                            results.push_back(CpuArch::unknown);
                            break;
                        }
                        // check if it is a DLL
                        if (!bundle){
                            uint16_t flags;
                            memcpy(&flags, &header[18], sizeof(flags));
                            if (!(flags & IMAGE_FILE_DLL)){
                                throw Error(Error::ModuleError, "not a DLL");
                            }
                        }
                    } else {
                        throw Error(Error::ModuleError, "bad PE signature");
                    }
                } else {
                    throw Error(Error::ModuleError, "DOS stub too large");
                }
            } else if (!bundle) {
                throw Error(Error::ModuleError, "not a DLL");
            }
        #elif defined(__APPLE__) // macOS (TODO: handle iOS?)
            // read Mach-O header
            auto read_uint32 = [](std::fstream& f, bool swap){
                uint32_t i;
                if (!f.read((char *)&i, sizeof(i))){
                    throw Error(Error::ModuleError, "end of file reached");
                }
                if (swap){
                    swap_bytes(i);
                }
                return i;
            };

            auto getCpuArch = [](cpu_type_t arch){
                switch (arch){
                // case CPU_TYPE_I386:
                case CPU_TYPE_X86:
                    return CpuArch::i386;
                case CPU_TYPE_X86_64:
                    return CpuArch::amd64;
                case CPU_TYPE_ARM:
                    return CpuArch::arm;
                case CPU_TYPE_ARM64:
                    return CpuArch::aarch64;
                case CPU_TYPE_POWERPC:
                    return CpuArch::ppc;
                case CPU_TYPE_POWERPC64:
                    return CpuArch::ppc64;
                default:
                    return CpuArch::unknown;
                }
            };

            auto readMachHeader = [&](std::fstream& f, bool swap, bool wide){
                LOG_DEBUG("reading mach-o header");
                cpu_type_t cputype = read_uint32(f, swap);
                uint32_t cpusubtype = read_uint32(f, swap); // ignored
                uint32_t filetype = read_uint32(f, swap);
                // check if it is a dylib or Mach-bundle
                if (!bundle && filetype != MH_DYLIB && filetype != MH_BUNDLE){
                    throw Error(Error::ModuleError, "not a plugin");
                }
                return getCpuArch(cputype);
            };

            auto readFatArchive = [&](std::fstream& f, bool swap, bool wide){
                LOG_DEBUG("reading fat archive");
                std::vector<CpuArch> archs;
                auto count = read_uint32(f, swap);
                for (auto i = 0; i < count; ++i){
                    // fat_arch is 20 bytes and fat_arch_64 is 32 bytes
                    // read CPU type
                    cpu_type_t arch = read_uint32(f, swap);
                    archs.push_back(getCpuArch(arch));
                    // skip remaining bytes. LATER also check file type.
                    if (wide){
                        char dummy[28];
                        f.read(dummy, sizeof(dummy));
                    } else {
                        char dummy[16];
                        f.read(dummy, sizeof(dummy));
                    }
                }
                return archs;
            };

            uint32_t magic = 0;
            file.read((char *)&magic, sizeof(magic));

            // *_CIGAM tells us to swap endianess
            switch (magic){
            case MH_MAGIC:
                results = { readMachHeader(file, false, false) };
                break;
            case MH_CIGAM:
                results = { readMachHeader(file, true, false) };
                break;
        #ifdef MH_MAGIC_64
            case MH_MAGIC_64:
                results = { readMachHeader(file, false, true) };
                break;
            case MH_CIGAM_64:
                results = { readMachHeader(file, true, true) };
                break;
        #endif
            case FAT_MAGIC:
                results = readFatArchive(file, false, false);
                break;
            case FAT_CIGAM:
                results = readFatArchive(file, true, false);
                break;
        #ifdef FAT_MAGIC_64
            case FAT_MAGIC_64:
                results = readFatArchive(file, false, true);
                break;
            case FAT_CIGAM_64:
                results = readFatArchive(file, true, true);
                break;
        #endif
            default:
                // on macOS, VST plugins are always bundles, so we just ignore non-plugin files
                break;
            }
        #else // Linux, FreeBSD, etc. (TODO: handle Android?)
            // read ELF header
            // check magic number
            char data[64]; // ELF header size
            if (file.read(data, sizeof(data)) && !memcmp(data, ELFMAG, SELFMAG)){
                char endian = data[0x05];
                int byteorder;
                if (endian == ELFDATA2LSB){
                    byteorder = LITTLE_ENDIAN;
                } else if (endian == ELFDATA2MSB){
                    byteorder = BIG_ENDIAN;
                } else {
                    throw Error(Error::ModuleError, "invalid data encoding in ELF header");
                }
                // check file type
                uint16_t filetype;
                memcpy(&filetype, &data[0x10], sizeof(filetype));
                if (BYTE_ORDER != byteorder){
                    swap_bytes(filetype);
                }
                // check if it is a shared object
                if (!bundle && filetype != ET_DYN){
                    throw Error(Error::ModuleError, "not a shared object");
                }
                // read CPU architecture
                uint16_t arch;
                memcpy(&arch, &data[0x12], sizeof(arch));
                if (BYTE_ORDER != byteorder){
                    swap_bytes(arch);
                }
                switch (arch){
                case EM_386:
                    results.push_back(CpuArch::i386);
                    break;
                case EM_X86_64:
                    results.push_back(CpuArch::amd64);
                    break;
                case EM_PPC:
                    results.push_back(CpuArch::ppc);
                    break;
                case EM_PPC64:
                    results.push_back(CpuArch::ppc64);
                    break;
                case EM_ARM:
                    results.push_back(CpuArch::arm);
                    break;
                case EM_AARCH64:
                    results.push_back(CpuArch::aarch64);
                    break;
                default:
                    results.push_back(CpuArch::unknown);
                    break;
                }
            } else if (!bundle) {
                throw Error(Error::ModuleError, "not a shared object");
            }
        #endif
        } else {
            if (!bundle){
                throw Error(Error::ModuleError, "couldn't open file");
            } else {
                LOG_ERROR("couldn't open " << path);
            }
        }
    }
    return results;
}
} // detail

std::vector<CpuArch> getCpuArchitectures(const std::string& path){
    auto results = detail::getCpuArchitectures(path, false);
    if (results.empty()) {
        // this code path is only reached by bundles
        throw Error(Error::ModuleError, "bundle doesn't contain any plugins");
    }
    return results;
}

void printCpuArchitectures(const std::string& path){
    auto archs = getCpuArchitectures(path);
    if (!archs.empty()){
        std::stringstream ss;
        for (auto& arch : archs){
            ss << getCpuArchString(arch) << " ";
        }
        LOG_VERBOSE("CPU architectures: " << ss.str());
    } else {
        LOG_VERBOSE("CPU architectures: none");
    }
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

// recursively search a directory for VST plugins. for every plugin, 'fn' is called with the full absolute path.
void search(const std::string &dir, std::function<void(const std::string&)> fn, bool filter) {
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
                auto path = entry.path();
                auto ext = path.extension().u8string();
                if (extensions.count(ext)) {
                    // found a VST2 plugin (file or bundle)
                    fn(path.u8string());
                } else if (fs::is_directory(path)){
                    // otherwise search it if it's a directory
                    searchDir(path);
                } else if (!filter && fs::is_regular_file(path)){
                    fn(path.u8string());
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
                if (extPos != std::string::npos){
                    ext = name.substr(extPos);
                }
                if (extensions.count(ext)){
                    // found a VST2 plugin (file or bundle)
                    fn(absPath);
                } else if (isDirectory(dirname, entry)){
                    // otherwise search it if it's a directory
                    searchDir(absPath);
                } else if (!filter && isFile(absPath)){
                    fn(absPath);
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
    bool checkThread(){
    #ifdef _WIN32
        return Win32::UIThread::checkThread();
    #elif defined(__APPLE__)
        return Cocoa::UIThread::checkThread();
    #elif defined(USE_X11)
        return X11::UIThread::checkThread();
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

void setThreadPriority(ThreadPriority p){
#ifdef _WIN32
    auto thread = GetCurrentThread();
    // set the thread priority
    if (SetThreadPriority(thread, (p == ThreadPriority::High) ?
                          THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_LOWEST)){
        // disable priority boost
        if (!SetThreadPriorityBoost(thread, (p == ThreadPriority::High) ? FALSE : TRUE)){
            LOG_WARNING("couldn't disable thread priority boost");
        }
    } else {
        LOG_WARNING("couldn't set thread priority");
    }
#else
    // set priority
    // high priority value taken from Pd, see s_inter.c
    struct sched_param param;
    param.sched_priority = (p == ThreadPriority::High) ? sched_get_priority_max(SCHED_FIFO) - 7 : 0;
    if (pthread_setschedparam(pthread_self(), (p == ThreadPriority::High) ? SCHED_FIFO : SCHED_OTHER, &param) != 0){
        LOG_WARNING("couldn't set thread priority");
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
#endif

#ifdef __APPLE__
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
#endif

#if DL_OPEN
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
#ifdef _WIN32
    return std::make_unique<ModuleWin32>(path);
#endif
#if defined __APPLE__
    return std::make_unique<ModuleApple>(path);
#endif
#if DL_OPEN
    return std::make_unique<ModuleSO>(path);
#endif
    return nullptr;
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
        if (!pathExists(path)){
            throw Error(Error::ModuleError, "No such file");
        }
        auto arch = getCpuArchitectures(path);
        if (std::find(arch.begin(), arch.end(), getHostCpuArchitecture()) == arch.end()){
            // TODO try bridging
            throw Error(Error::ModuleError, "Wrong CPU architecture");
        }
        return std::make_shared<VST3Factory>(path);
    #else
        throw Error(Error::ModuleError, "VST3 plug-ins not supported");
    #endif
    } else {
    #if USE_VST2
        std::string realPath = path;
        if (path.find(ext) == std::string::npos){
            realPath += ext;
        }
        if (!pathExists(realPath)){
            throw Error(Error::ModuleError, "No such file");
        }
        auto arch = getCpuArchitectures(realPath);
        if (std::find(arch.begin(), arch.end(), getHostCpuArchitecture()) == arch.end()){
            // TODO try bridging
            throw Error(Error::ModuleError, "Wrong CPU architecture");
        }
        return std::make_shared<VST2Factory>(realPath);
    #else
        throw Error(Error::ModuleError, "VST2 plug-ins not supported");
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
IFactory::ProbeResultFuture IFactory::probePlugin(const std::string& name, int shellPluginID) {
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
    std::wstring probePath = getModuleDirectory() + L"\\probe.exe";
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

    if (!CreateProcessW(probePath.c_str(), &cmdLine[0], NULL, NULL,
                        PROBE_LOG, DETACHED_PROCESS, NULL, NULL, &si, &pi)){
        auto err = GetLastError();
        std::stringstream ss;
        ss << "couldn't open probe process (" << errorMessage(err) << ")";
        throw Error(Error::SystemError, ss.str());
    }
    auto wait = [pi](){
        if (WaitForSingleObject(pi.hProcess, INFINITE) != 0){
            throw Error(Error::SystemError, "couldn't wait for probe process!");
        }
        DWORD code = -1;
        if (!GetExitCodeProcess(pi.hProcess, &code)){
            throw Error(Error::SystemError, "couldn't retrieve exit code for probe process!");
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
        throw Error(Error::SystemError, "couldn't get module path!");
    }
    std::string modulePath = dlinfo.dli_fname;
    auto end = modulePath.find_last_of('/');
    std::string probePath = modulePath.substr(0, end) + "/probe";
    // fork
    pid_t pid = fork();
    if (pid == -1) {
        throw Error(Error::SystemError, "fork() failed!");
    }
    else if (pid == 0) {
        // child process: start new process with plugin path and temp file path as arguments.
        // we must not quote arguments to exec!
    #if !PROBE_LOG
        // disable stdout and stderr
        auto nullOut = fopen("/dev/null", "w");
        fflush(stdout);
        dup2(fileno(nullOut), STDOUT_FILENO);
        fflush(stderr);
        dup2(fileno(nullOut), STDERR_FILENO);
    #endif
        if (execl(probePath.c_str(), "probe", path().c_str(), pluginName.c_str(), tmpPath.c_str(), nullptr) < 0) {
            // write error to temp file
            int err = errno;
            File file(tmpPath, File::WRITE);
            if (file.is_open()){
                file << static_cast<int>(Error::SystemError) << "\n";
                file << "couldn't open probe process (" << errorMessage(err) << ")\n";
            }
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
        ProbeResult result;
        result.plugin = std::move(desc);
        result.total = 1;
        auto ret = wait(); // wait for process to finish
        /// LOG_DEBUG("return code: " << ret);
        TmpFile file(tmpPath); // removes the file on destruction
        if (ret == EXIT_SUCCESS) {
            // get info from temp file
            if (file.is_open()) {
                desc->deserialize(file);
            }
            else {
                result.error = Error(Error::SystemError, "couldn't read temp file!");
            }
        }
        else if (ret == EXIT_FAILURE) {
            // get error from temp file
            if (file.is_open()) {
                int code;
                std::string msg;
                file >> code;
                if (!file){
                    // happens in certain cases, e.g. the plugin destructor
                    // terminates the probe process with exit code 1.
                    code = (int)Error::UnknownError;
                }
                std::getline(file, msg); // skip newline
                std::getline(file, msg); // read message
                LOG_DEBUG("code: " << code << ", msg: " << msg);
                result.error = Error((Error::ErrorCode)code, msg);
            }
            else {
                result.error = Error(Error::SystemError, "couldn't read temp file!");
            }
        }
        else {
            // ignore temp file
            result.error = Error(Error::Crash);
        }
        return result;
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
        const ProbeList& pluginList, ProbeCallback callback){
    // shell plugin!
    int numPlugins = pluginList.size();
    std::vector<PluginInfo::ptr> results;
#ifdef PLUGIN_LIMIT
    numPlugins = std::min<int>(numPlugins, PLUGIN_LIMIT);
#endif
#if !PROBE_THREADS
    /// LOG_DEBUG("numPlugins: " << numPlugins);
    std::vector<std::pair<int, ProbeResultFuture>> futures;
    int i = 0;
    while (i < numPlugins){
        futures.clear();
        // probe the next n plugins
        int n = std::min<int>(numPlugins - i, PROBE_FUTURES);
        for (int j = 0; j < n; ++j, ++i){
            auto& name = pluginList[i].first;
            auto& id = pluginList[i].second;
            /// LOG_DEBUG("probing '" << name << "'");
            try {
                futures.emplace_back(i, probePlugin(name, id));
            } catch (const Error& e){
                // return error future
                futures.emplace_back(i, [=](){
                    ProbeResult result;
                    result.error = e;
                    return result;
                });
            }
        }
        // collect results
        for (auto& it : futures) {
            auto result = it.second(); // wait on future
            result.index = it.first;
            result.total = numPlugins;
            if (result.valid()) {
                results.push_back(result.plugin);
            }
            if (callback){
                callback(result);
            }
        }
    }
#else
    DEBUG_THREAD("numPlugins: " << numPlugins);
    std::vector<ProbeResult> probeResults;
    int head = 0;
    int tail = 0;

    std::mutex mutex;
    std::condition_variable cond;
    int numThreads = std::min<int>(numPlugins, PROBE_THREADS);
    std::vector<std::thread> threads;

    // thread function
    auto threadFun = [&](int i){
        DEBUG_THREAD("worker thread " << i << " started");
        std::unique_lock<std::mutex> lock(mutex);
        while (head < numPlugins){
            auto& name = pluginList[head].first;
            auto& id = pluginList[head].second;
            head++;
            lock.unlock();

            DEBUG_THREAD("thread " << i << ": probing '" << name << "'");
            ProbeResult result;
            try {
                result = probePlugin(name, id)(); // call future
            } catch (const Error& e){
                DEBUG_THREAD("probe error " << e.what());
                result.error = e;
            }

            lock.lock();
            probeResults.push_back(result);
            DEBUG_THREAD("thread " << i << ": probed " << name);
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
    DEBUG_THREAD("collect results");
    while (true) {
        // process available data
        while (tail < (int)probeResults.size()){
            auto result = probeResults[tail]; // copy!
            lock.unlock();

            result.index = tail++;
            result.total = numPlugins;
            if (result.valid()) {
                results.push_back(result.plugin);
                DEBUG_THREAD("got plugin " << result.plugin->name
                    << " (" << (result.index + 1) << " of " << numPlugins << ")");
            }
            if (callback){
                callback(result);
            }

            lock.lock();
        }
        // wait for more data if needed
        if ((int)probeResults.size() < numPlugins){
            DEBUG_THREAD("wait...");
            cond.wait(lock);
        } else {
            break;
        }
    }
    lock.unlock(); // !!!

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

/*////////////////////// preset ///////////////////////*/

static SharedMutex gFileLock;
#if !defined(_WIN32) && !defined(__APPLE__)
static bool gDidCreateVstFolders = false;
#endif

static std::string getPresetLocation(PresetType presetType, PluginType pluginType){
    std::string result;
#if defined(_WIN32)
    switch (presetType){
    case PresetType::User:
        result = expandPath("%USERPROFILE%") + "\\Documents";
        break;
    case PresetType::UserFactory:
        result = expandPath("%APPDATA%");
        break;
    case PresetType::SharedFactory:
        result = expandPath("%PROGRAMDATA%");
        break;
    default:
        return "";
    }
    if (pluginType == PluginType::VST3){
        return result + "\\VST3 Presets";
    } else {
        return result + "\\VST2 Presets";
    }
#elif defined(__APPLE__)
    switch (presetType){
    case PresetType::User:
        return expandPath("~/Library/Audio/Presets");
    case PresetType::SharedFactory:
        return "/Library/Audio/Presets";
    default:
        return "";
    }
#else
    switch (presetType){
    case PresetType::User:
    {
        result = expandPath("~/.");
        // VST directories might not exist yet
        SharedLock rdlock(gFileLock);
        if (!gDidCreateVstFolders){
            rdlock.unlock();
            Lock wrlock(gFileLock);
            // LATER do some error handling
        #if USE_VST2
            createDirectory(result + "vst");
        #endif
        #if USE_VST3
            createDirectory(result + "vst3");
        #endif
            gDidCreateVstFolders = true;
        }
        break;
    }
    case PresetType::SharedFactory:
        result = "/usr/local/share/";
        break;
    case PresetType::Global:
        result = "/usr/share/";
        break;
    default:
        return "";
    }
    if (pluginType == PluginType::VST3){
        return result + "vst3/presets";
    } else {
        return result + "vst/presets";
    }
#endif
}

/*///////////////////// PluginInfo /////////////////////*/

PluginInfo::PluginInfo(const std::shared_ptr<const IFactory>& factory)
    : path(factory->path()), factory_(factory) {}

IPlugin::ptr PluginInfo::create() const {
    std::shared_ptr<const IFactory> factory = factory_.lock();
    return factory ? factory->create(name) : nullptr;
}

#if USE_VST2
void PluginInfo::setUniqueID(int _id){
    type_ = PluginType::VST2;
    char buf[9];
    // should we write in little endian?
    snprintf(buf, sizeof(buf), "%08X", _id);
    buf[8] = 0;
    uniqueID = buf;
    id_.id = _id;
}
#endif

#if USE_VST3
void PluginInfo::setUID(const char *uid){
    type_ = PluginType::VST3;
    char buf[33];
    for (int i = 0; i < sizeof(TUID); ++i){
        // we have to cast to uint8_t!
        sprintf(buf + 2 * i, "%02X", (uint8_t)uid[i]);
    }
    buf[32] = 0;
    uniqueID = buf;
    memcpy(id_.uid, uid, 16);
}
#endif

static void conformPath(std::string& path){
    // replace backslashes
    for (auto& c : path){
        if (c == '\\'){
            c = '/';
        }
    }
}

void PluginInfo::scanPresets(){
    const std::vector<PresetType> presetTypes = {
#if defined(_WIN32)
        PresetType::User, PresetType::UserFactory, PresetType::SharedFactory
#elif defined(__APPLE__)
        PresetType::User, PresetType::SharedFactory
#else
        PresetType::User, PresetType::SharedFactory, PresetType::Global
#endif
    };
    PresetList results;
    for (auto& presetType : presetTypes){
        auto folder = getPresetFolder(presetType);
        if (pathExists(folder)){
            vst::search(folder, [&](const std::string& path){
                auto ext = fileExtension(path);
                if ((type_ == PluginType::VST3 && ext != "vstpreset") ||
                    (type_ == PluginType::VST2 && ext != "fxp" && ext != "FXP")){
                    return;
                }
                Preset preset;
                preset.type = presetType;
                preset.name = fileBaseName(path);
                preset.path = path;
            #ifdef _WIN32
                conformPath(preset.path);
            #endif
                results.push_back(std::move(preset));
            }, false);
        }
    }
    presets = std::move(results);
    sortPresets(false);
#if 0
    if (numPresets()){
        LOG_VERBOSE("presets:");
        for (auto& preset : presets){
            LOG_VERBOSE("\t" << preset.path);
        }
    }
#endif
}

bool stringCompare(const std::string& lhs, const std::string& rhs){
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](const auto& c1, const auto& c2){ return std::tolower(c1) < std::tolower(c2); }
    );
}

void PluginInfo::sortPresets(bool userOnly){
    // don't lock! private method
    auto it1 = presets.begin();
    auto it2 = it1;
    if (userOnly){
        // get iterator past user presets
        while (it2 != presets.end() && it2->type == PresetType::User) ++it2;
    } else {
        it2 = presets.end();
    }
    std::sort(it1, it2, [](const auto& lhs, const auto& rhs) {
        return stringCompare(lhs.name, rhs.name);
    });
}

int PluginInfo::findPreset(const std::string &name) const {
    for (int i = 0; i < presets.size(); ++i){
        if (presets[i].name == name){
            return i;
        }
    }
    return -1;
}

bool PluginInfo::removePreset(int index, bool del){
    if (index >= 0 && index < presets.size()
            && presets[index].type == PresetType::User
            && (!del || removeFile(presets[index].path))){
        presets.erase(presets.begin() + index);
        return true;
    }
    return false;
}

bool PluginInfo::renamePreset(int index, const std::string& newName){
    // make preset before creating the lock!
    if (index >= 0 && index < presets.size()
            && presets[index].type == PresetType::User){
        auto preset = makePreset(newName);
        if (!preset.name.empty()){
            if (renameFile(presets[index].path, preset.path)){
                presets[index] = std::move(preset);
                sortPresets();
                return true;
            }
        }
    }
    return false;
}

static std::string bashPath(std::string path){
    for (auto& c : path){
        switch (c){
        case '/':
        case '\\':
        case '\"':
        case '?':
        case '*':
        case ':':
        case '<':
        case '>':
        case '|':
            c = '_';
            break;
        default:
            break;
        }
    }
    return path;
}

int PluginInfo::addPreset(Preset preset) {
    auto it = presets.begin();
    // insert lexicographically
    while (it != presets.end() && it->type == PresetType::User){
        if (preset.name == it->name){
            // replace
            *it = std::move(preset);
            return (int)(it - presets.begin());
        }
        if (stringCompare(preset.name, it->name)){
            break;
        }
        ++it;
    }
    int index = (int)(it - presets.begin()); // before reallocation!
    presets.insert(it, std::move(preset));
    return index;
}

Preset PluginInfo::makePreset(const std::string &name, PresetType type) const {
    Preset preset;
    auto folder = getPresetFolder(type, true);
    if (!folder.empty()){
        preset.path = folder + "/" + bashPath(name) +
                (type_ == PluginType::VST3 ? ".vstpreset" : ".fxp");
        preset.name = name;
        preset.type = type;
    }
    return preset;
}

std::string PluginInfo::getPresetFolder(PresetType type, bool create) const {
    auto location = getPresetLocation(type, type_);
    if (!location.empty()){
        auto vendorFolder = location + "/" + bashPath(vendor);
        auto pluginFolder = vendorFolder + "/" + bashPath(name);
        // create folder(s) if necessary
        if (create && !didCreatePresetFolder && type == PresetType::User){
            // LATER do some error handling
            createDirectory(location);
            createDirectory(vendorFolder);
            createDirectory(pluginFolder);
            didCreatePresetFolder = true;
        }
    #ifdef _WIN32
        conformPath(pluginFolder);
    #endif
        return pluginFolder;
    } else {
        return "";
    }
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

namespace {

std::string ltrim(std::string str){
    auto pos = str.find_first_not_of(" \t");
    if (pos != std::string::npos && pos > 0){
        return str.substr(pos);
    } else {
        return str;
    }
}

std::string rtrim(std::string str){
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

void getKeyValuePair(const std::string& line, std::string& key, std::string& value){
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

} // namespace

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
                        type_ = PluginType::VST2;
                        sscanf(&value[0], "%08X", &id_.id);
                    } else if (value.size() == 32){
                        type_ = PluginType::VST3;
                        for (int i = 0; i < 16; ++i){
                            unsigned int temp;
                            sscanf(&value[i * 2], "%02X", &temp);
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


