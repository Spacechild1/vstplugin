#pragma once

#include <vector>
#include <string>

namespace vst {

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

CpuArch cpuArchFromString(const std::string& name);

std::vector<CpuArch> getCpuArchitectures(const std::string& path);

void printCpuArchitectures(const std::string& path);

} // vst
