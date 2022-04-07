#pragma once

#include "Interface.h"

#ifdef _WIN32
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#endif

namespace vst {

//--------------- ProcessHandle ------------------//

class ProcessHandle {
public:
#ifdef _WIN32
    ProcessHandle() {
        ZeroMemory(&pi_, sizeof(pi_));
    }

    ProcessHandle(const PROCESS_INFORMATION& pi)
        : pi_(pi) {}

    ~ProcessHandle() {
        close();
    }

    ProcessHandle(ProcessHandle&& other) {
        pi_ = other.pi_;
        other.pi_.dwProcessId = 0; // sentinel
    }

    ProcessHandle& operator=(ProcessHandle&& other) {
        pi_ = other.pi_;
        other.pi_.dwProcessId = 0; // sentinel
        return *this;
    }
#else
    ProcessHandle() : pid_(-1) {}

    ProcessHandle(int pid)
        : pid_(pid) {}

    ProcessHandle(ProcessHandle&& other) {
        pid_ = other.pid_;
        other.pid_ = -1; // sentinel
    }

    ProcessHandle& operator=(ProcessHandle&& other) {
        pid_ = other.pid_;
        other.pid_ = -1; // sentinel
        return *this;
    }
#endif
    int pid() const;

    int wait();

    bool valid() const;

    operator bool() const { return valid(); }

    std::pair<bool, int> tryWait(double timeout);

    bool checkIfRunning();

    bool terminate();
private:
#ifdef _WIN32
    PROCESS_INFORMATION pi_;
    void close();
#else
    int pid_;
    int parseStatus(int status);
#endif
};

//-------------------- IHostApp -----------------------//

enum class CpuArch;

class IHostApp {
public:
    static IHostApp* get(CpuArch arch);

    virtual ~IHostApp() {}

    virtual CpuArch arch() const = 0;

    virtual const std::string& path() const = 0;

    virtual ProcessHandle probe(const std::string& path, int id,
                                const std::string& tmpPath) const = 0;

    virtual ProcessHandle bridge(const std::string& shmPath, intptr_t logPipe) const = 0;
};

} // vst
