#include "MiscUtils.h"
#include "Log.h"
#include "FileUtils.h"
#include "CpuArch.h"

#ifdef _WIN32
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#else
# include <stdlib.h>
# include <string.h>
# include <signal.h>
# include <dlfcn.h>
# include <unistd.h>
# include <fcntl.h>
#endif

#if VST_HOST_SYSTEM != VST_WINDOWS
// thread priority
#include <pthread.h>
#endif

#include <mutex>
#include <sstream>

namespace vst {

static LogFunction gLogFunction = nullptr;

void setLogFunction(LogFunction f){
    gLogFunction = f;
}

void logMessage(int level, const char *msg){
    if (gLogFunction){
        gLogFunction(level, msg);
    } else {
        std::cerr << msg;
        std::flush(std::cerr);
    }
}

void logMessage(int level, const std::string& msg){
    if (gLogFunction){
        gLogFunction(level, msg.c_str());
    } else {
        std::cerr << msg;
        std::flush(std::cerr);
    }
}

//-----------------------------------------------------------------//

template<typename T>
void doBypass(ProcessData &data){
    for (int i = 0; i < data.numOutputs; ++i){
        auto output = (T **)data.outputs[i].channelData32;
        auto nout = data.outputs[i].numChannels;
        if (i < data.numInputs){
            auto input = (T **)data.inputs[i].channelData32;
            auto nin = data.inputs[i].numChannels;
            for (int j = 0; j < nout; ++j){
                auto out = output[j];
                if (j < nin){
                    // copy input to output
                    auto in = input[j];
                    std::copy(in, in + data.numSamples, out);
                } else {
                    // zero channel
                    std::fill(out, out + data.numSamples, 0);
                }
            }
        } else {
            // zero whole bus
            for (int j = 0; j < nout; ++j){
                auto out = output[j];
                std::fill(out, out + data.numSamples, 0);
            }
        }
    }
}

void bypass(ProcessData &data){
    if (data.precision == ProcessPrecision::Double){
        doBypass<double>(data);
    } else {
        doBypass<float>(data);
    }
}

//-----------------------------------------------------------------//

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
    // omit trailing newlines or carriage returns
    auto ptr = buf + size - 1;
    while (size-- && (*ptr == '\r' || *ptr == '\n')){
        *ptr-- = '\0';
    }
    ss << shorten(buf);
#else
    ss << strerror(err);
#endif
    // include error number
    ss << " [" << err << "]";
    return ss.str();
}

//-------------------------------------------------------------//

#ifdef _WIN32

static HINSTANCE hInstance = 0;

const std::wstring& getModuleDirectory(){
    static std::wstring dir = [](){
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
            LOG_ERROR("getModuleDirectory: GetModuleFileNameW() failed!");
            return std::wstring();
        }
    }();
    return dir;
}

extern "C" {
    BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved){
        if (fdwReason == DLL_PROCESS_ATTACH){
            hInstance = hinstDLL;
        }
        return TRUE;
    }
}

#else // Linux, macOS

const std::string& getModuleDirectory(){
    static std::string dir = [](){
        // hack: obtain library info through a function pointer (vst::search)
        Dl_info dlinfo;
        if (!dladdr((void *)search, &dlinfo)) {
            throw Error(Error::SystemError, "getModuleDirectory: dladdr() failed!");
        }
        std::string path = dlinfo.dli_fname;
        auto end = path.find_last_of('/');
        return path.substr(0, end);
    }();
    return dir;
}

#endif // WIN32

std::string getHostApp(CpuArch arch){
    if (arch == getHostCpuArchitecture()){
    #ifdef _WIN32
        return "host.exe";
    #else
        return "host";
    #endif
    } else {
        std::string host = std::string("host_") + cpuArchToString(arch);
    #if defined(_WIN32)
        host += ".exe";
    #endif
        return host;
    }
}

#if !defined(_WIN32) && !defined(__WINE__)
# define SUPPRESS_OUTPUT 1
#else
# define SUPPRESS_OUTPUT 0
#endif

#if SUPPRESS_OUTPUT

// disableOutput() and restoreOutput()
// are never called concurrently!
static int stdoutfd;
static int stderrfd;

void disableOutput() {
    stdoutfd = dup(STDOUT_FILENO);
    stderrfd = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    close(devnull);
}

void restoreOutput() {
    dup2(stdoutfd, STDOUT_FILENO);
    dup2(stderrfd, STDERR_FILENO);
    close(stdoutfd);
    close(stderrfd);
}

#endif // SUPPRESS_OUTPUT

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

bool haveWine(){
    // we only need to do this once!
    static bool _haveWine = [](){
        auto winecmd = getWineCommand();
        // we pass valid arguments, so the exit code should be 0.
        char cmdline[256];
        snprintf(cmdline, sizeof(cmdline), "\"%s\" --version", winecmd);

        fflush(stdout);
    #if SUPPRESS_OUTPUT
        disableOutput();
    #endif

        auto status = system(cmdline);

        fflush(stdout);
    #if SUPPRESS_OUTPUT
        restoreOutput();
    #endif

        if (WIFEXITED(status)){
            auto code = WEXITSTATUS(status);
            if (code == EXIT_SUCCESS){
                LOG_DEBUG("'" << winecmd << "' command is working");
                return true; // success
            } else if (code == EXIT_FAILURE) {
                LOG_VERBOSE("'" << winecmd << "' command failed or not available");
            } else {
                LOG_ERROR("'" << winecmd << "' command failed with exit code " << code);
            }
        } else if (WIFSIGNALED(status)) {
            LOG_ERROR("'" << winecmd << "' command was terminated with signal " << WTERMSIG(status));
        } else {
            LOG_ERROR("'" << winecmd << "' command failed with status " << status);
        }
        return false;
    }();
    return _haveWine;
}

#endif

#if USE_BRIDGE
// Generally, we can bridge between any kinds of CPU architectures,
// as long as the they are supported by the platform in question.
//
// We use the following naming scheme for the plugin bridge app:
// host_<cpu_arch>[extension]
// Examples: "host_i386", "host_amd64.exe", etc.
//
// We can selectively enable/disable CPU architectures simply by
// including resp. omitting the corresponding app.
// Note that we always ship a version of the *same* CPU architecture
// called "host" resp. "host.exe" to support plugin sandboxing.
//
// Bridging between i386 and amd64 is typically employed on Windows,
// but also possible on Linux and macOS (before 10.15).
// On the upcoming ARM MacBooks, we can also bridge between amd64 and aarch64.
// NOTE: We ship 64-bit Intel builds on Linux without "host_i386" and
// ask people to compile it themselves if they need it.
//
// On macOS and Linux we can also use the plugin bridge to run Windows plugins
// via Wine. The apps are called "host_pe_i386.exe" and "host_pe_amd64.exe".

std::mutex gHostAppMutex;

std::unordered_map<CpuArch, bool> gHostAppDict;

bool canBridgeCpuArch(CpuArch arch) {
    std::lock_guard<std::mutex> lock(gHostAppMutex);
    auto it = gHostAppDict.find(arch);
    if (it != gHostAppDict.end()){
        return it->second;
    }

#ifdef _WIN32
    auto path = shorten(getModuleDirectory()) + "\\" + getHostApp(arch);
#else // Unix
  #if USE_WINE
    if (arch == CpuArch::pe_i386 || arch == CpuArch::pe_amd64){
        // check if the 'wine' command can be found and works.
        if (!haveWine()){
            gHostAppDict[arch] = false;
            return false;
        }
    }
  #endif // USE_WINE
    auto path = getModuleDirectory() + "/" + getHostApp(arch);
#endif
    // check if host app exists and works
    if (pathExists(path)){
    #if defined(_WIN32) && !defined(__WINE__)
        std::wstringstream ss;
        ss << L"\"" << widen(path) << L"\" test";
        fflush(stdout);
        auto code = _wsystem(ss.str().c_str());
    #else // Unix
        char cmdline[256];
      #if USE_WINE
        if (arch == CpuArch::pe_i386 || arch == CpuArch::pe_amd64){
            snprintf(cmdline, sizeof(cmdline),
                     "\"%s\" \"%s\" test", getWineCommand(), path.c_str());
        } else
      #endif
        {
            snprintf(cmdline, sizeof(cmdline), "\"%s\" test", path.c_str());
        }

        fflush(stdout);
    #if SUPPRESS_OUTPUT
        disableOutput();
    #endif

        auto status = system(cmdline);

        fflush(stdout);
    #if SUPPRESS_OUTPUT
        restoreOutput();
    #endif

        if (!WIFEXITED(status)) {
            if (WIFSIGNALED(status)) {
                LOG_ERROR("host app '" << path
                          << "' was terminated with signal " << WTERMSIG(status));
            } else {
                LOG_ERROR("host app '" << path << "' failed with status " << status);
            }
            gHostAppDict[arch] = false;
            return false;
        }
        auto code = WEXITSTATUS(status);
    #endif
        if (code == EXIT_SUCCESS){
            LOG_DEBUG("host app '" << path << "' is working");
            gHostAppDict[arch] = true;
            return true; // success
        } else {
            LOG_ERROR("host app '" << path << "' failed with exit code " << code);
            gHostAppDict[arch] = false;
            return false;
        }
    } else {
        LOG_VERBOSE("can't find host app " << path);
        gHostAppDict[arch] = false;
        return false;
    }
}
#endif

//-------------------------------------------------------------//

void setProcessPriority(Priority p){
#if VST_HOST_SYSTEM == VST_WINDOWS
    auto priorityClass = (p == Priority::High) ?
                HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
    if (!SetPriorityClass(GetCurrentProcess(), priorityClass)){
        LOG_WARNING("couldn't set process priority");
    }
#endif
}

void setThreadPriority(Priority p){
#if VST_HOST_SYSTEM == VST_WINDOWS
    auto thread = GetCurrentThread();
    // set the thread priority
    auto threadPriority = (p == Priority::High) ?
                THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_LOWEST;
    if (SetThreadPriority(thread, threadPriority)){
        // disable priority boost
        if (!SetThreadPriorityBoost(thread, (p != Priority::High))){
            LOG_WARNING("couldn't disable thread priority boost");
        }
    } else {
        LOG_WARNING("couldn't set thread priority");
    }
#else
    // set priority
    // high priority value taken from Pd, see s_inter.c
    auto policy = (p == Priority::High) ? SCHED_FIFO : SCHED_OTHER;
    struct sched_param param;
    param.sched_priority = (p == Priority::High) ?
                sched_get_priority_max(SCHED_FIFO) - 7 : 0;
    if (pthread_setschedparam(pthread_self(), policy, &param) != 0){
        LOG_WARNING("couldn't set thread priority");
    }
#endif
}

} // vst
