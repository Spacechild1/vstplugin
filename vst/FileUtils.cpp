#include "FileUtils.h"
#include "Interface.h"
#include "MiscUtils.h"
#include "Log.h"

#ifdef _WIN32
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#else
# include <sys/stat.h>
#endif

#if USE_STDFS
# include <filesystem>
namespace fs = std::filesystem;
#elif VST_HOST_SYSTEM != VST_WINDOWS
# include <dirent.h>
# include <unistd.h>
# include <strings.h>
# include <sys/wait.h>
# include <sys/stat.h>
# include <sys/types.h>
# ifndef ACCESSPERMS
#  define ACCESSPERMS (S_IRWXU|S_IRWXG|S_IRWXO)
# endif
#else
# error "must use std::filesystem on Windows!"
#endif

namespace vst {

#ifdef _WIN32

std::string expandPath(const char *path) {
    wchar_t buf[MAX_PATH];
    ExpandEnvironmentStringsW(widen(path).c_str(), buf, MAX_PATH);
    return shorten(buf);
}

std::string userSettingsPath() {
    return expandPath("%LOCALAPPDATA%\\vstplugin");
}

#else

std::string expandPath(const char *path) {
    // only expands ~ to home directory so far
    if (*path == '~') {
        const char *home = getenv("HOME");
        if (home) {
            return std::string(home) + std::string(path + 1);
        }
    }
    return path;
}

std::string userSettingsPath() {
    auto config = getenv("XDG_DATA_HOME");
    if (config) {
        return std::string(config) + "/vstplugin";
    } else {
    #ifdef __APPLE__
        return expandPath("~/Library/Application Support/vstplugin");
    #else
        return expandPath("~/.local/share/vstplugin");
    #endif
    }
}

#endif

// 1. use uniform directory separator ('/')
// 2. remove '/./' components
// 3. handle and remove '/../' components
std::string normalizePath(const std::string &path) {
    std::string result;
    result.reserve(path.size()); // result will always be equal or smaller

#ifdef _WIN32
    auto start = path.find_first_of("/\\");
#else
    auto start = path.find_first_of('/');
#endif
    if (start == std::string::npos) {
        // no separators
        return path;
    }
    // append everything before the first separator
    result.append(path, 0, start);
    // start at first separator
    for (int i = start; i < path.size(); ++i) {
        auto c = path[i];
    #ifdef _WIN32
        if (c == '\\') {
            c = '/';
        }
    #endif
        if (c == '/') {
            auto is_separator = [](auto c) {
            #ifdef _WIN32
                return c == '/' || c == '\\';
            #else
                return c == '/';
            #endif
            };
            auto is_dot = [](auto c) {
                return c == '.';
            };
            // first skip redundant separators
            while (is_separator(path[i + 1])) {
                i++;
            }
            // then look for '/./' or '/../'
            if (is_dot(path[i + 1])) {
                if (is_separator(path[i + 2])) {
                    // skip '/.' and continue with following '/'
                    i++;
                    continue;
                } else if (is_dot(path[i + 2]) && is_separator(path[i + 3])) {
                    // pop previous directory (if there is any)
                    auto last = result.find_last_of('/');
                    if (last != std::string::npos) {
                        result.erase(last);
                    }
                    // skip '/..' and continue with following '/'
                    i += 2;
                    continue;
                }
                // '.' is part of a file/directory name
            }
        }
        result.push_back(c);
    }
    LOG_DEBUG("normalized path " << path << " to " << result);
    return result;
}

bool pathExists(const std::string& path){
#if USE_STDFS
    std::error_code e;
    return fs::exists(widen(path), e);
#else
    struct stat stbuf;
    auto ret = stat(path.c_str(), &stbuf);
    if (ret == 0){
        return true;
    } else {
    #if 0
        LOG_DEBUG(path << ": stat failed: "
                  << errorMessage(errno));
    #endif
        return false;
    }
#endif
}

bool isFile(const std::string& path){
#if USE_STDFS
    return fs::is_regular_file(widen(path));
#else
    struct stat stbuf;
    return (stat(path.c_str(), &stbuf) == 0) && S_ISREG(stbuf.st_mode);
#endif
}

bool isDirectory(const std::string& path){
#if USE_STDFS
    std::error_code e;
    return fs::is_directory(widen(path), e);
#else
    struct stat stbuf;
    return (stat(path.c_str(), &stbuf) == 0) && S_ISDIR(stbuf.st_mode);
#endif
}

bool removeFile(const std::string& path){
#if USE_STDFS
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
        LOG_ERROR(errorMessage(errno));
        return false;
    } else {
        return true;
    }
#endif
}

bool renameFile(const std::string& from, const std::string& to){
#if USE_STDFS
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
#if USE_STDFS
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

std::string fileDirectory(const std::string& path){
#ifdef _WIN32
    auto pos = path.find_last_of("/\\");
#else
    auto pos = path.find_last_of('/');
#endif
    if (pos != std::string::npos){
        return path.substr(0, pos);
    } else {
        return path;
    }
}

// include the dot!
std::string fileExtension(const std::string& path){
    auto name = fileName(path);
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos){
        return name.substr(dot);
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

// return the timestamp of the last modification (file content changed, file replaced, etc.)
// as a Unix timestamp (number of seconds since Jan 1, 1970).
#ifdef _WIN32
double fileTimeLastModified(const std::string &path) {
    HANDLE hFile = CreateFileW(widen(path).c_str(),
                               GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        throw Error(Error::SystemError, "CreateFile() failed: " + errorMessage(GetLastError()));
    }
    FILETIME creationTime, writeTime;
    if (!GetFileTime(hFile, &creationTime, NULL, &writeTime)) {
        CloseHandle(hFile);
        throw Error(Error::SystemError, "GetFileTime() failed: " + errorMessage(GetLastError()));
    }
    CloseHandle(hFile);
    uint64_t ct = ((uint64_t)creationTime.dwHighDateTime << 32) | (creationTime.dwLowDateTime);
    uint64_t wt = ((uint64_t)writeTime.dwHighDateTime << 32) | (writeTime.dwLowDateTime);
    // use the newer timestamp
    auto t = std::max<uint64_t>(ct, wt);
    // Kudos to https://www.frenk.com/2009/12/convert-filetime-to-unix-timestamp/
    // Between Jan 1, 1601 and Jan 1, 1970 there are 11644473600 seconds.
    // FILETIME uses 100-nanosecond intervals.
    return (t * 0.0000001) - 11644473600;
}
#else
double fileTimeLastModified(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        throw Error(Error::SystemError, "stat() failed: " + errorMessage(errno));
    }
    // macOS does not support nanosecond precision!
#ifdef __APPLE__
    double mtime = st.st_mtime;
    double ctime = st.st_ctime;
#else
    double mtime = (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec * 0.000000001;
    double ctime = (double)st.st_ctim.tv_sec + st.st_ctim.tv_nsec * 0.000000001;
#endif
    // return the newer timestamp
    return std::max<double>(mtime, ctime);
}
#endif

//---------------------------------------------------//

File::File(const std::string& path, Mode mode)
#if (VST_HOST_SYSTEM == VST_WINDOWS) && (defined(_MSC_VER) || __GNUC__ >= 9)
// UTF-16 file names supported by MSVC and newer GCC versions
// NOTE: doesn't work on Wine!
    : std::fstream(vst::widen(path).c_str(),
#else
// might create problems on Windows... LATER fix this
    : std::fstream(path,
#endif
                   ios_base::binary |
                   (mode == READ ? ios_base::in : (ios_base::out | ios_base::trunc))) {}

TmpFile::TmpFile(const std::string& path, Mode mode)
    : File(path, mode), path_(path) {}

TmpFile::~TmpFile(){
    if (is_open()){
        close();
        // destructor must not throw!
        if (!removeFile(path_)){
            LOG_ERROR("couldn't remove tmp file!");
        };
    }
}

} // vst
