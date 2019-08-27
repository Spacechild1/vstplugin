#include "Interface.h"
#include "Utility.h"
#include <stdlib.h>
#include <fstream>

using namespace vst;

#ifdef _MSC_VER
#include <windows.h>
#endif

#define NO_STDOUT 1
#define NO_STDERR 1

#ifdef _WIN32
 #if NO_STDOUT || NO_STDERR
  #include <stdio.h>
  #include <io.h>
  #define DUP(fd) _dup(fd)
  #define DUP2(fd, newfd) _dup2(fd, newfd)
  const char *nullFileName = "NUL";
  #ifdef _MSC_VER
   #define STDOUT_FILENO _fileno(stdout)
   #define STDERR_FILENO _fileno(stderr)
   #define fileno _fileno
  #endif
 #endif
 #define MAIN wmain
 #define CHAR wchar_t
#else
 #if NO_STDOUT || NO_STDERR
  #include <stdio.h>
  #include <unistd.h>
  #define DUP(fd) dup(fd)
  #define DUP2(fd, newfd) dup2(fd, newfd)
  const char *nullFileName = "/dev/null";
 #endif
 #define MAIN main
 #define CHAR char
 #define shorten(x) x
#endif

// probe a plugin and write info to file
// returns EXIT_SUCCESS on success, EXIT_FAILURE on fail and everything else on error/crash :-)
#ifdef _MSC_VER
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd){
    int argc;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
#else
int MAIN(int argc, const CHAR *argv[]) {
#endif
    int status = EXIT_FAILURE;
    // surpress stdout and/or stderr
#if NO_STDOUT || NO_STDERR
    auto nullOut = fopen(nullFileName, "w");
#endif
#if NO_STDOUT
    fflush(stdout);
    DUP2(fileno(nullOut), STDOUT_FILENO);
#endif
#if NO_STDERR
    fflush(stderr);
    DUP2(fileno(nullOut), STDERR_FILENO);
#endif
    if (argc >= 3){
		const CHAR *pluginPath = argv[1];
		const CHAR *pluginName = argv[2];
		const CHAR *filePath = argc > 3 ? argv[3] : nullptr;
        LOG_DEBUG("probe.exe: " << shorten(pluginPath) << ", " << shorten(pluginName));
        try {
            auto factory = vst::IFactory::load(shorten(pluginPath));
            auto plugin = factory->create(shorten(pluginName), true);
            if (filePath) {
                vst::File file(shorten(filePath), File::WRITE);
                if (file.is_open()) {
                    plugin->info().serialize(file);
                } else {
                    LOG_ERROR("probe: couldn't write info file");
                }
            }
            status = EXIT_SUCCESS;
            LOG_DEBUG("probe succeeded");
        } catch (const std::exception& e){
            // catch any exception!
            LOG_DEBUG("probe failed: " << e.what());
        }
	}
#if NO_STDOUT || NO_STDERR
    if (nullOut){
        fclose(nullOut);
    }
#endif
#ifdef _MSC_VER
    LocalFree(argv); // let's be nice
#endif
    return status;
}
