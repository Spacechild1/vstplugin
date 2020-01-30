#include <Utility.h>

#ifdef _WIN32
# include <windows.h>
# include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
// just because of Clang on macOS doesn't ship <experimental/filesystem>...
# include <unistd.h>
# include <string.h>
# include <sys/stat.h>
# include <sys/types.h>
# ifndef ACCESSPERMS
#  define ACCESSPERMS (S_IRWXU|S_IRWXG|S_IRWXO)
# endif
# include <pthread.h>
#endif
#include <sstream>

namespace vst {

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
#endif

bool stringCompare(const std::string& lhs, const std::string& rhs){
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](const auto& c1, const auto& c2){ return std::tolower(c1) < std::tolower(c2); }
    );
}

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

} // vst
