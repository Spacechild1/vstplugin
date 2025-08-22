#pragma once

#include <string>
#include <fstream>

namespace vst {

std::string expandPath(const char *path);

std::string normalizePath(const std::string& path);

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

double fileTimeLastModified(const std::string& path);

// Cross platform fstream, taking UTF-8 file paths.
// Will become obsolete when we ditch macOS versions below 10.15.
class File : public std::fstream {
public:
    enum Mode {
        READ,
        WRITE
    };
    File(const std::string& path, Mode mode = READ);

    std::string readAll();
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
