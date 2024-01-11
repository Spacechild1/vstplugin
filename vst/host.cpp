#include "Interface.h"
#include "PluginDesc.h"
#include "Log.h"
#include "FileUtils.h"
#include "MiscUtils.h"
#if USE_BRIDGE
#include "PluginServer.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <mutex>

using namespace vst;

#ifndef USE_WMAIN
# ifdef _WIN32
#  define USE_WMAIN 1
# else
#  define USE_WMAIN 0
# endif
#endif

#if !USE_WMAIN
# define shorten(x) x
#endif

void writeErrorMsg(Error::ErrorCode code, const char* msg, const std::string& path){
    if (!path.empty()){
        // this part should be async signal safe
        vst::File file(path, File::WRITE);
        if (file.is_open()) {
            file << static_cast<int>(code) << "\n";
            file << msg << "\n";
        } else {
            LOG_ERROR("ERROR: couldn't write error message");
        }
    }
}

// probe a plugin and write info to file
// returns EXIT_SUCCESS on success, EXIT_FAILURE on fail and anything else on error/crash :-)
int probe(const std::string& pluginPath, int pluginIndex, const std::string& filePath)
{
    setThreadPriority(Priority::Low);

    LOG_DEBUG("probing " << pluginPath << " " << pluginIndex);
    try {
        auto factory = vst::IFactory::load(pluginPath, true);
        auto desc = factory->probePlugin(pluginIndex);
        if (!filePath.empty()) {
            vst::File file(filePath, File::WRITE);
            if (file.is_open()) {
                desc->serialize(file);
            } else {
                LOG_ERROR("ERROR: couldn't write info file " << filePath);
            }
        }
        LOG_VERBOSE("probe succeeded");
        return EXIT_SUCCESS;
    } catch (const Error& e){
        writeErrorMsg(e.code(), e.what(), filePath);
        LOG_ERROR("probe failed: " << e.what());
    } catch (const std::exception& e) {
        writeErrorMsg(Error::UnknownError, e.what(), filePath);
        LOG_ERROR("probe failed: " << e.what());
    } catch (...) {
        writeErrorMsg(Error::UnknownError, "unknown exception", filePath);
        LOG_ERROR("probe failed: unknown exception");
    }
    return EXIT_FAILURE;
}

#if USE_BRIDGE

#if VST_HOST_SYSTEM == VST_WINDOWS
static HANDLE gLogChannel = NULL;
#else
static int gLogChannel = -1;
#endif

#ifdef _WIN32
namespace vst {
    void setParentProcess(int pid); // WindowWin32.cpp
}
#endif

static std::mutex gLogMutex;

void writeLog(int level, const char *msg){
    LogMessage::Header header;
    header.level = level;
    header.size = strlen(msg) + 1;
#if VST_HOST_SYSTEM == VST_WINDOWS
    if (gLogChannel) {
        std::lock_guard<std::mutex> lock(gLogMutex);
        DWORD bytesWritten;
        WriteFile(gLogChannel, &header, sizeof(header), &bytesWritten, NULL);
        WriteFile(gLogChannel, msg, header.size, &bytesWritten, NULL);
    }
#else
    if (gLogChannel >= 0) {
        std::lock_guard<std::mutex> lock(gLogMutex);
        write(gLogChannel, &header, sizeof(header));
        write(gLogChannel, msg, header.size);
    }
#endif
}

// host one or more VST plugins
int bridge(int pid, const std::string& path, int logChannel){
#ifdef _WIN32
    setParentProcess(pid); // see WindowWin32.cpp
#endif
#if VST_HOST_SYSTEM == VST_WINDOWS
    // setup log channel
    auto hParent = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    if (hParent){
        if (DuplicateHandle(hParent, (HANDLE)(uintptr_t)logChannel,
                            GetCurrentProcess(), &gLogChannel,
                            0, FALSE, DUPLICATE_SAME_ACCESS)) {
            setLogFunction(writeLog);
        } else {
            LOG_ERROR("DuplicateHandle() failed: " << errorMessage(GetLastError()));
        }
    } else {
        LOG_ERROR("OpenProcess() failed: " << errorMessage(GetLastError()));
    }
    if (hParent) {
        CloseHandle(hParent);
    }
#else
    gLogChannel = logChannel;
    setLogFunction(writeLog);
#endif

    LOG_DEBUG("bridge begin");
    // main thread is UI thread
    setThreadPriority(Priority::Low);
    try {
        // create and run server
        PluginServer server(pid, path);

        server.run();

        LOG_DEBUG("bridge end");

        return EXIT_SUCCESS;
    } catch (const Error& e){
        // LATER redirect stderr to parent stdin to get the error message
        LOG_ERROR(e.what());
        return EXIT_FAILURE;
    }
}

#endif // USE_BRIDGE

#if USE_WMAIN
int wmain(int argc, const wchar_t *argv[]){
#else
int main(int argc, const char *argv[]) {
#endif
    if (argc >= 2){
        std::string verb = shorten(argv[1]);
        argc -= 2;
        argv += 2;
        if (verb == "probe" && argc > 0){
            // args: <plugin_path> [<id>] [<file_path>] [<timeout>]
            std::string path = shorten(argv[0]);
            int index = -1;
            if (argc > 1) {
                try {
                    index = std::stol(argv[1], 0, 0);
                } catch (...) {} // non-numeric argument, e.g. '_'
            }
            std::string file = argc > 2 ? shorten(argv[2]) : "";

            return probe(path, index, file);
        }
    #if USE_BRIDGE
        else if (verb == "bridge" && argc >= 3){
            // args: <pid> <shared_mem_path> <log_pipe>
            int pid, logChannel;
            try {
                pid = std::stol(argv[0], 0, 0);
            } catch (...) {
                LOG_ERROR("bad 'pid' argument: " << argv[0]);
                return EXIT_FAILURE;
            }
            std::string shmPath = shorten(argv[1]);
            try {
                logChannel = std::stol(argv[2], 0, 0);
            } catch (...) {
                LOG_ERROR("bad 'log_pipe' argument: " << argv[2]);
                return EXIT_FAILURE;
            }
            return bridge(pid, shmPath, logChannel);
        }
    #endif
        else if (verb == "test" && argc > 0){
            std::string version = shorten(argv[0]);
            // version must match exactly
            if (version == getVersionString()) {
                return EXIT_SUCCESS;
            } else {
                LOG_ERROR("version mismatch");
                return EXIT_FAILURE;
            }
        } else if (verb == "--version") {
            std::cout << "vstplugin " << getVersionString() << std::endl;
            return EXIT_SUCCESS;
        }
    }
    std::cout << "usage:\n"
              << "  probe <plugin_path> [<id>] [<file_path>]\n"
#if USE_BRIDGE
              << "  bridge <pid> <shared_mem_path> <log_pipe>\n"
#endif
              << "  test <version>\n"
              << "  --version" << std::endl;
    return EXIT_FAILURE;
}
