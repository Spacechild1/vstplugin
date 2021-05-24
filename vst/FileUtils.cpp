#include "FileUtils.h"

#include "MiscUtils.h"
#include "Log.h"

#ifdef _WIN32
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#endif

#if USE_STDFS
# include <filesystem>
namespace fs = std::filesystem;
# ifndef _WIN32
#  define widen(x) x
# endif
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

//---------------------------------------------------//

File::File(const std::string& path, Mode mode)
#if defined(_WIN32) && (defined(_MSC_VER) || __GNUC__ >= 9) && !defined(__WINE__)
// UTF-16 file names supported by MSVC and newer GCC versions
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
