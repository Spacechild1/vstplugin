#pragma once

#include "Interface.h"

#ifdef _WIN32
#include <windows.h>
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
        if (pi_.hProcess) {
            CloseHandle(pi_.hProcess);
            CloseHandle(pi_.hThread);
        }
    }

    ProcessHandle(ProcessHandle&& other) {
        pi_ = other.pi_;
        other.pi_.hProcess = NULL;
        other.pi_.hThread = NULL;
    }

    ProcessHandle& operator=(ProcessHandle&& other) {
        pi_ = other.pi_;
        other.pi_.hProcess = NULL;
        other.pi_.hThread = NULL;
        return *this;
    }
#else
    ProcessHandle() : pid_(-1) {}

    ProcessHandle(int pid)
        : pid_(pid) {}

    ProcessHandle(ProcessHandle&& other) = default;

    ProcessHandle& operator=(ProcessHandle&& other) = default;
#endif
    int pid() const;

    int wait();

    std::pair<bool, int> tryWait(double timeout);

    bool checkIfRunning();

    bool terminate();
private:
#ifdef _WIN32
    PROCESS_INFORMATION pi_;
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
