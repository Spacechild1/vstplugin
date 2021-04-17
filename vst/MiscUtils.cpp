#include "MiscUtils.h"
#include "Log.h"

#ifdef _WIN32
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#else
# include <stdlib.h>
# include <string.h>
# include <signal.h>
#endif

#if VST_HOST_SYSTEM != VST_WINDOWS
// thread priority
#include <pthread.h>
#endif

#include <sstream>

namespace vst {

using LogFunction = void(*)(const char *);

static LogFunction gLogFunction = nullptr;

void setLogFunction(LogFunction f){
    gLogFunction = f;
}

Log::~Log(){
    stream_ << "\n";
    std::string msg = stream_.str();
    if (gLogFunction){
        gLogFunction(msg.c_str());
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

#ifndef _WIN32
#define MATCH(x) case x: return #x;
const char *strsignal(int sig){
    switch (sig) {
    MATCH(SIGINT)
    MATCH(SIGILL)
    MATCH(SIGABRT)
    MATCH(SIGFPE)
    MATCH(SIGSEGV)
    MATCH(SIGTERM)
    MATCH(SIGHUP)
    MATCH(SIGQUIT)
    MATCH(SIGTRAP)
    MATCH(SIGKILL)
    MATCH(SIGBUS)
    MATCH(SIGSYS)
    MATCH(SIGPIPE)
    MATCH(SIGALRM)
    default:
        return "unknown";
    }
}
#undef MATCH
#endif

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
