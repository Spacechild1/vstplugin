#include "FileUtils.h"
#include "Log.h"

#include <cstdlib>
#include <vector>

std::vector<std::pair<std::string, std::string>> pathList = {
    { "C:/Foo/Bar/test.exe", "C:/Foo/Bar/test.exe" },
    { "C:/Foo/Bar/./test.exe", "C:/Foo/Bar/test.exe" },
    { "C:/Foo/../Bar/./test.exe", "C:/Bar/test.exe" },
    { "C:/../Foo/Bar/./test.exe", "C:/Foo/Bar/test.exe" },
    { "C:/../../Foo/Bar/./test.exe", "C:/Foo/Bar/test.exe" },
    { "C://Foo///Bar/////test.exe", "C:/Foo/Bar/test.exe" },
    { "C:/Foo//Bar/////test.exe", "C:/Foo/Bar/test.exe" },
    { "C:/Foo/././Bar/././././test.exe", "C:/Foo/Bar/test.exe" },
    { "C:/./Foo/././Bar/./test.exe", "C:/Foo/Bar/test.exe" },
    { "/Foo/Bar/./test.exe", "/Foo/Bar/test.exe" },
    { "/Foo/./Bar/./test.exe", "/Foo/Bar/test.exe" },
    { "/Foo/../Bar/./test.exe", "/Bar/test.exe" },
    { "/../Foo/Bar/./test.exe", "/Foo/Bar/test.exe" },
    { "/../../Foo/Bar/./test.exe", "/Foo/Bar/test.exe" },
    { "/Foo/../../Bar/./test.exe", "/Bar/test.exe" },
    { "/Foo/Bar/Baz/../../test.exe", "/Foo/test.exe" },
    { "/Foo/Bar/Baz/../../../test.exe", "/test.exe" },
    { "//Foo///Bar/////test.exe", "/Foo/Bar/test.exe" },
    { "/Foo//Bar/////test.exe", "/Foo/Bar/test.exe" },
    { "/./Foo/././Bar/././././test.exe", "/Foo/Bar/test.exe" },
    { "/Foo/././Bar/././././test.exe", "/Foo/Bar/test.exe" }
};

int main(int argc, const char *argv[]) {
    for (auto& [test, expected] : pathList) {
        auto normalized = vst::normalizePath(test);
        if (normalized != expected) {
            LOG_ERROR("path " << normalized << " (normalized from "
                      << test << ") does not match " << expected);
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
