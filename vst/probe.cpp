#include <stdlib.h>

// returns 0 on success, 1 on fail and everything else on error/crash :-)

#ifdef _WIN32
extern "C" int probe(const wchar_t *pluginPath, const wchar_t *filePath);

int wmain(int argc, const wchar_t *argv[]){
    if (argc >= 2){
        // plugin path + temp file path (if given)
        return probe(argv[1], argc >= 3 ? argv[2] : nullptr) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    return EXIT_FAILURE;
}
#else
extern "C" int probe(const char *pluginPath, const char *filePath);

int main(int argc, const char *argv[]) {
	if (argc >= 2) {
		// plugin path + temp file path (if given)
		return probe(argv[1], argc >= 3 ? argv[2] : nullptr) ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	return EXIT_FAILURE;
}
#endif


