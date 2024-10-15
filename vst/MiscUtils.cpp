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

void logMessage(int level, const char* msg){
    if (gLogFunction){
        gLogFunction(level, msg);
    } else {
        std::cerr << msg;
        std::flush(std::cerr);
    }
}

void logMessage(int level, std::string_view msg){
    if (gLogFunction){
        gLogFunction(level, msg.data());
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
std::wstring widen(std::string_view s){
    if (s.empty()){
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), s.size(), NULL, 0);
    std::wstring buf;
    buf.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), s.size(), &buf[0], n);
    return buf;
}

std::string shorten(std::wstring_view s){
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

const std::string& getModuleDirectory(){
    static std::string dir = [](){
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
            return shorten(wpath);
        } else {
            LOG_ERROR("GetModuleFileNameW() failed!");
            return std::string();
        }
    }();
    return dir;
}

void * getModuleHandle() {
    return (void *)hInstance;
}

int getCurrentProcessId() {
    return GetCurrentProcessId();
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

int getCurrentProcessId() {
    return getpid();
}

#endif // WIN32

#ifdef _WIN32
int runCommand(std::string_view cmd, std::string_view args) {
    auto wcmd = widen(cmd);
    fflush(stdout);
    // don't use system() or _wspawnl() to avoid console popping up
#if 1
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    std::stringstream cmdline;
    // NOTE: to be 100% safe, let's quote the command
    cmdline << "\"" << fileName(std::string{cmd}) << "\" " << args;
    auto wcmdline = widen(cmdline.str());

    if (!CreateProcessW(wcmd.c_str(), &wcmdline[0], NULL, NULL, 0,
                        DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
        throw Error(Error::SystemError, errorMessage(GetLastError()));
    }

    auto res = WaitForSingleObject(pi.hProcess, INFINITE);
    if (res == WAIT_OBJECT_0){
        DWORD exitCode;
        auto success = GetExitCodeProcess(pi.hProcess, &exitCode);
        auto e = GetLastError(); // cache error before calling CloseHandle()!
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (success) {
            return exitCode;
        } else {
            throw Error(Error::SystemError,
                        "GetExitCodeProcess() failed: " + errorMessage(e));
        }
    } else {
        auto e = GetLastError();
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        throw Error(Error::SystemError,
                    "WaitForSingleObject() failed: " + errorMessage(e));
    }
#else
    // still shows console...
    // NOTE: to be 100% safe, let's quote the command
    auto quotecmd = L"\"" + widen(fileName(std::string{cmd})) + L"\"";
    auto wargs = widen(args);
    auto result = _wspawnl(_P_WAIT, wcmd.c_str(),
                           quotecmd.c_str(), wargs.c_str(), NULL);
    if (result >= 0) {
        return result;
    } else {
        auto e = errno;
        std::stringstream ss;
        ss << strerror(e) << " (" << e << ")";

        throw Error(Error::SystemError, ss.str());
    }
#endif
}

#else

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

int runCommand(std::string_view cmd, std::string_view args) {
    char cmdline[1024];
    snprintf(cmdline, sizeof(cmdline), "\"%s\" %s",
             cmd.data(), args.data());

    fflush(stdout);
#if SUPPRESS_OUTPUT
    disableOutput();
#endif

    auto status = system(cmdline);
    auto e = errno; // save errno!

    fflush(stdout);
#if SUPPRESS_OUTPUT
    restoreOutput();
#endif

    if (status >= 0) {
        if (WIFEXITED(status)) {
            // exited normally
            return WEXITSTATUS(status);
        } else {
            std::stringstream ss;
            if (WIFSIGNALED(status)) {
                ss << "terminated with signal " << WTERMSIG(status);
            } else {
                ss << "failed with status " << status;
            }
            throw Error(Error::SystemError, ss.str());
        }
    } else {
        // system() failed
        throw Error(Error::SystemError, errorMessage(e));
    }
}

#endif // _WIN32

#if USE_WINE
const char * getWineCommand(){
    // users can override the 'wine' command with the
    // 'WINELOADER' environment variable
    const char *cmd = getenv("WINELOADER");
    if (cmd) {
        LOG_DEBUG("WINELOADER = " << cmd);
        return cmd;
    } else {
        return "wine";
    }
}

const char * getWine64Command(){
    static auto cmd = []() -> std::string {
        auto commandExists = [](std::string_view cmd) {
            char buf[256];
            snprintf(buf, sizeof(buf), "which %s > /dev/null 2>&1", cmd.data());
            return system(buf) == 0;
        };
        // users can override the 'wine' command with the
        // 'WINELOADER' environment variable
        const char *cmd = getenv("WINELOADER");
        if (cmd) {
            LOG_DEBUG("WINELOADER = " << cmd);
            // try 64-bit variant
            auto wine64 = std::string(cmd) + "64";
            if (commandExists(wine64)) {
                return wine64;
            } else {
                return cmd;
            }
        } else {
            // Some wine distros don't have wine64 in their path.
            // Instead, 'wine' is just a shell script that redirects
            // to the actual 'wine' resp. 'wine64' binaries, e.g. in
            // /usr/lib/wine. The script may default to 'wine' because
            // the latter can actually handle both 64-bit and 32-bit apps,
            // However, this 'wine' launcher may need to fork a new process,
            // leading our watchdog to think that our subprocess has terminated!
            // That's why we try to find the actual 'wine64' loader.
            std::string wine64 = "wine64";
            if (commandExists(wine64)) {
                return wine64;
            }
            wine64 = "/usr/lib/wine/wine64";
            if (commandExists(wine64)) {
                return wine64;
            }
            LOG_WARNING("VSTPlugin: could not find 'wine64' command");
            return "wine"; // fallback
        }
    }();
    return cmd.c_str();
}

const char * getWineFolder(){
    // the default Wine folder is '~/.wine', but it can be
    // overridden with the 'WINEPREFIX' environment variable
    const char *prefix = getenv("WINEPREFIX");
    return prefix ? prefix : "~/.wine";
}

static bool testWine(const char *cmd) {
    try {
        // we pass valid arguments, so the exit code should be 0.
        auto code = runCommand(cmd, "--version");
        if (code == EXIT_SUCCESS){
            LOG_DEBUG("'" << cmd << "' command is working");
            return true; // success
        } else if (code == EXIT_FAILURE) {
            LOG_INFO("'" << cmd << "' command failed or not available");
        } else {
            LOG_ERROR("'" << cmd << "' command failed with exit code " << code);
        }
    } catch (const Error& e) {
        LOG_ERROR("'" << cmd << "' command failed :" << e.what());
    }
    return false;
}

bool haveWine(){
    // we only need to do this once!
    static bool result = testWine(getWineCommand());
    return result;
}

bool haveWine64(){
    // we only need to do this once!
    static bool result = testWine(getWine64Command());
    return result;
}

#endif

//-------------------------------------------------------------//

void setThreadPriority(Priority p){
#if VST_HOST_SYSTEM == VST_WINDOWS
    auto thread = GetCurrentThread();
    // set the thread priority
    int threadPriority;
    if (p == Priority::High) {
        // make it work independently from the process priority class
        threadPriority = THREAD_PRIORITY_TIME_CRITICAL;
    } else if (p == Priority::Low) {
        threadPriority = THREAD_PRIORITY_LOWEST;
    } else {
        threadPriority = THREAD_PRIORITY_NORMAL;
    }
    if (SetThreadPriority(thread, threadPriority)){
    #if 0
        // disable priority boost
        if (!SetThreadPriorityBoost(thread, (p == Priority::Low))){
            LOG_WARNING("VSTPlugin: couldn't disable thread priority boost");
        }
    #endif
    } else {
        LOG_WARNING("VSTPlugin: couldn't set thread priority");
    }
#else
    // set priority
    // high priority value taken from Pd, see s_inter.c
    auto policy = (p == Priority::High) ? SCHED_FIFO : SCHED_OTHER;
    struct sched_param param;
    // TODO: Low vs Normal
    param.sched_priority = (p == Priority::High) ?
                sched_get_priority_max(SCHED_FIFO) - 7 : 0;
    if (pthread_setschedparam(pthread_self(), policy, &param) != 0){
        LOG_WARNING("VSTPlugin: couldn't set thread priority");
    }
#endif
}

} // vst
