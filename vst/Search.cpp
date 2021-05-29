#include "Interface.h"

#include "MiscUtils.h"
#include "FileUtils.h"
#include "Log.h"

#if USE_STDFS
# include <filesystem>
namespace fs = std::filesystem;
# ifndef _WIN32
#  define widen(x) x
# endif
#else
# include <dirent.h>
# include <unistd.h>
# include <strings.h>
# include <sys/wait.h>
# include <sys/stat.h>
# include <sys/types.h>
#endif

#include <unordered_set>
#include <cstring>
#include <algorithm>

namespace vst {

static const std::vector<const char *> platformExtensions = {
#if USE_VST2
 #if defined(_WIN32) || USE_WINE // Windows or Wine
    ".dll",
 #endif // Windows or Wine
 #ifndef _WIN32 // Unix
  #ifdef __APPLE__
    ".vst",
  #else
    ".so",
  #endif
 #endif // Unix
#endif // VST2
#if USE_VST3
    ".vst3",
#endif // VST3
};

const std::vector<const char *>& getPluginExtensions() {
    return platformExtensions;
}

bool hasPluginExtension(const std::string& path){
    auto ext = fileExtension(path);
    for (auto& e : platformExtensions){
        if (ext == e){
            return true;
        }
    }
    return false;
}

const char * getBundleBinaryPath(){
    auto path =
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
# if USE_BRIDGE
   // native folder first (in case we try to find a single plugin)
#  ifdef _WIN64 // 64 bit
#    define PROGRAMFILES(x) "%ProgramW6432%\\" x, "%ProgramFiles(x86)%\\" x
#  else // 32 bit
#    define PROGRAMFILES(x) "%ProgramFiles(x86)%\\" x, "%ProgramW6432%\\" x
#  endif
# else
#  ifdef _WIN64 // 64 bit
#   define PROGRAMFILES(x) "%ProgramFiles%\\" x
#  else // 32 bit
#   define PROGRAMFILES(x) "%ProgramFiles(x86)%\\" x
#  endif
# endif // USE_BRIDGE
#endif // WIN32

static const std::vector<const char *> defaultSearchPaths = {
/*---- VST2 ----*/
#if USE_VST2
    // macOS
  #ifdef __APPLE__
    "~/Library/Audio/Plug-Ins/VST", "/Library/Audio/Plug-Ins/VST",
  #endif
    // Windows
  #ifdef _WIN32
    PROGRAMFILES("VSTPlugins"), PROGRAMFILES("Steinberg\\VSTPlugins"),
    PROGRAMFILES("Common Files\\VST2"), PROGRAMFILES("Common Files\\Steinberg\\VST2"),
  #endif
    // Linux
  #ifdef __linux__
    "~/.vst", "/usr/local/lib/vst", "/usr/lib/vst",
  #endif
#endif
/*---- VST3 ----*/
#if USE_VST3
    // macOS
  #ifdef __APPLE__
    "~/Library/Audio/Plug-Ins/VST3", "/Library/Audio/Plug-Ins/VST3"
  #endif
    // Windows
  #ifdef _WIN32
    PROGRAMFILES("Common Files\\VST3")
  #endif
    // Linux
  #ifdef __linux__
    "~/.vst3", "/usr/local/lib/vst3", "/usr/lib/vst3"
  #endif
#endif // VST3
};

#if USE_WINE
// 64-bit folder first (in case we try to find a single plugin)
# define PROGRAMFILES(x) "/drive_c/Program Files/" x, "/drive_c/Program Files (x86)/" x

static std::vector<const char *> defaultWineSearchPaths = {
#if USE_VST2
    PROGRAMFILES("VSTPlugins"), PROGRAMFILES("Steinberg/VSTPlugins"),
    PROGRAMFILES("Common Files/VST2"), PROGRAMFILES("Common Files/Steinberg/VST2"),
#endif
#if USE_VST3
    PROGRAMFILES("Common Files/VST3")
#endif
};
#endif

#ifdef PROGRAMFILES
#undef PROGRAMFILES
#endif

// get "real" default search paths
const std::vector<std::string>& getDefaultSearchPaths() {
    static std::vector<std::string> result = [](){
        std::vector<std::string> list;
        for (auto& path : defaultSearchPaths) {
            list.push_back(expandPath(path));
        }
    #if USE_WINE
        std::string winePrefix = expandPath(getWineFolder());
        for (auto& path : defaultWineSearchPaths) {
            list.push_back(winePrefix + path);
        }
    #endif
        return list;
    }();
    return result;
}

#if USE_WINE
const char * getWineCommand(){
    // users can override the 'wine' command with the
    // 'WINELOADER' environment variable
    const char *cmd = getenv("WINELOADER");
    return cmd ? cmd : "wine";
}

const char * getWineFolder(){
    // the default Wine folder is '~/.wine', but it can be
    // overridden with the 'WINEPREFIX' environment variable
    const char *prefix = getenv("WINEPREFIX");
    return prefix ? prefix : "~/.wine";
}
#endif

#if !USE_STDFS
// helper function
static bool isDirectory(const std::string& fullPath, dirent *entry){
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
        if (stat(fullPath.c_str(), &stbuf) == 0){
            return S_ISDIR(stbuf.st_mode);
        }
    }
    return false;
}
#endif

// recursively search for a VST plugin in a directory. returns empty string on failure.
std::string find(const std::string &dir, const std::string &path){
    if (!pathExists(dir)){
        LOG_DEBUG("find: '" << dir << "' doesn't exist");
        return std::string{};
    }
    std::string relpath = path;
    // if the path has no file extension, assume VST2 plugin
    if (fileExtension(relpath).empty()){
    #ifdef _WIN32
        relpath += ".dll";
    #elif defined(__APPLE__)
        relpath += ".vst";
    #else // Linux/BSD/etc.
        relpath += ".so";
    #endif
    }
    LOG_DEBUG("try to find " << relpath << " in " << dir);
#if USE_STDFS
    try {
        auto wdir = widen(dir);
        auto fpath = fs::path(widen(relpath));
        auto file = fs::path(wdir) / fpath;
        if (fs::exists(file)){
            return file.u8string(); // success
        }
        // continue recursively
        auto options = fs::directory_options::follow_directory_symlink;
        for (auto& entry : fs::recursive_directory_iterator(wdir, options)) {
            if (fs::is_directory(entry)){
                file = entry.path() / fpath;
                if (fs::exists(file)){
                    return file.u8string(); // success
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        LOG_WARNING(e.what());
    };
    return std::string{};
#else // USE_STDFS
    auto root = dir;
    // remove trailing slashes
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }

    auto file = root + "/" + relpath;
    if (pathExists(file)){
        return file; // success
    }

    // continue recursively
    std::string result;

    std::function<void(const std::string&)> searchDir = [&](const std::string& dirname) {
        DIR *directory = opendir(dirname.c_str());
        if (directory){
            struct dirent *entry;
            while (result.empty() && (entry = readdir(directory))){
                std::string dir = dirname + "/" + entry->d_name;
                if (isDirectory(dir, entry)){
                    std::string fullPath = dir + "/" + relpath;
                    if (pathExists(fullPath)){
                        result = fullPath;
                        break;
                    } else {
                        // search directory
                        searchDir(dir);
                    }
                }
            }
            closedir(directory);
        }
    };

    searchDir(root);

    return result;
#endif // USE_STDFS
}

#if USE_STDFS

class PathList {
public:
    PathList(const std::vector<std::string>& paths){
        for (auto& path : paths){
            paths_.emplace_back(widen(path));
        }
    }
    bool contains(const fs::path& path) const {
        std::error_code e;
        return std::find_if(paths_.begin(), paths_.end(), [&](auto& p){
            return fs::equivalent(p, path, e);
        }) != paths_.end();
    }
    bool contains(const std::string& path) const {
        return contains(fs::path(widen(path)));
    }
private:
    std::vector<fs::path> paths_;
};

#else

class PathList {
public:
    PathList(const std::vector<std::string>& paths){
        for (auto& path : paths){
            struct stat buf;
            if (stat(path.c_str(), &buf) == 0){
                paths_.emplace_back(buf.st_ino);
            }
        }
    }
    bool contains(const std::string& path) const {
        struct stat buf;
        if (stat(path.c_str(), &buf) == 0){
            return contains(buf.st_ino);
        } else {
            return false;
        }
    }
    bool contains(struct dirent *dir) const {
        return contains(dir->d_ino);
    }
private:
    bool contains(ino_t inode) const {
        return std::find_if(paths_.begin(), paths_.end(), [&](auto& in){
            return in == inode;
        }) != paths_.end();
    }
    std::vector<ino_t> paths_;
};

#endif

// recursively search a directory for VST plugins. for every plugin, 'fn' is called with the full absolute path.
void search(const std::string &dir, std::function<void(const std::string&)> fn,
            bool filterByExtension, const std::vector<std::string>& excludePaths) {
    if (!pathExists(dir)){
        // LOG_DEBUG("search: '" << dir << "' doesn't exist");
        return;
    }

    PathList excludeList(excludePaths);
    if (excludeList.contains(dir)){
        LOG_DEBUG("search: ignore '" << dir << "'");
        return;
    }

#if USE_STDFS
  #ifdef _WIN32
    std::function<void(const std::wstring&)>
  #else
    std::function<void(const std::string&)>
  #endif
    searchDir = [&](const auto& dirname){
        try {
            // LOG_DEBUG("searching in " << shorten(dirname));
            auto options = fs::directory_options::follow_directory_symlink;
            fs::directory_iterator iter(dirname, options);
            for (auto& entry : iter) {
                auto& path = entry.path();

                if (excludeList.contains(path)){
                    LOG_DEBUG("search: ignore '" << path.u8string() << "'");
                    continue;
                }

                // check the extension
                if (hasPluginExtension(path.u8string())){
                    // found a VST plugin file or bundle
                    fn(path.u8string());
                } else if (fs::is_directory(path)){
                    // otherwise search it if it's a directory
                    searchDir(path);
                } else if (!filterByExtension && fs::is_regular_file(path)){
                    fn(path.u8string());
                }
            }
        } catch (const fs::filesystem_error& e) {
            LOG_WARNING(e.what());
        };
    };
    searchDir(widen(dir));
#else // USE_STDFS
    std::function<void(const std::string&)> searchDir = [&](const std::string& dirname) {
        // LOG_DEBUG("searching in " << dirname);
        // search alphabetically (ignoring case)
        struct dirent **dirlist;
        auto sortnocase = [](const struct dirent** a, const struct dirent **b) -> int {
            return strcasecmp((*a)->d_name, (*b)->d_name);
        };
        int n = scandir(dirname.c_str(), &dirlist, NULL, sortnocase);
        if (n >= 0) {
            for (int i = 0; i < n; ++i) {
                auto entry = dirlist[i];
                std::string path = dirname + "/" + entry->d_name;

                if (excludeList.contains(entry)){
                    LOG_DEBUG("search: ignore '" << path << "'");
                    continue;
                }

                // check the extension
                if (hasPluginExtension(path)){
                    // found a VST2 plugin (file or bundle)
                    fn(path);
                } else if (isDirectory(path, entry)){
                    // otherwise search it if it's a directory
                    searchDir(path);
                } else if (!filterByExtension && isFile(path)){
                    fn(path);
                }
                free(entry);
            }
            free(dirlist);
        }
    };

    auto root = dir;
    // removing trailing slashes
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }

    searchDir(root);
#endif // USE_STDFS
}

} // vst
