#pragma once

#include <string>
#include <fstream>

namespace vst {

std::string expandPath(const char *path);

std::string userSettingsPath();

bool pathExists(const std::string& path);

bool isDirectory(const std::string& path);

bool isFile(const std::string& path);

bool removeFile(const std::string& path);

bool renameFile(const std::string& from, const std::string& to);

bool createDirectory(const std::string& dir);

std::string fileName(const std::string& path);

std::string fileDirectory(const std::string& path);

std::string fileExtension(const std::string& path);

std::string fileBaseName(const std::string& path);

// cross platform fstream, taking UTF-8 file paths.
// will become obsolete when we can switch the whole project to C++17
class File : public std::fstream {
public:
    enum Mode {
        READ,
        WRITE
    };
    File(const std::string& path, Mode mode = READ);
};

// RAII class for automatic cleanup
class TmpFile : public File {
public:
    TmpFile(const std::string& path, Mode mode = READ);
    ~TmpFile();
protected:
    std::string path_;
};

} // vst
