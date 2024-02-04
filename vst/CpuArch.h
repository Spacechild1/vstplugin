#pragma once

#include <vector>
#include <string>

namespace vst {

// defined by 32-bit GCC; this may cause compiler errors!
#undef i386

enum class CpuArch {
    unknown,
    amd64,
    i386,
    arm,
    aarch64,
    ppc,
    ppc64,
#ifndef _WIN32
    // PE executables (for Wine support)
    pe_i386,
    pe_amd64
#endif
};

CpuArch getHostCpuArchitecture();

const char * cpuArchToString(CpuArch arch);

CpuArch cpuArchFromString(std::string_view name);

std::vector<CpuArch> getPluginCpuArchitectures(const std::string& path);

std::vector<CpuArch> getFileCpuArchitectures(const std::string& path);

void printCpuArchitectures(const std::string& path);

} // vst
