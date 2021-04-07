#include "Interface.h"
#include "Utility.h"

#ifdef _WIN32
#include <Windows.h>
// IMAGE_FILE_MACHINE_ARM64 is only defined on Windows 8.1 and above
#ifndef IMAGE_FILE_MACHINE_ARM64
 #define IMAGE_FILE_MACHINE_ARM64 0xaa64
#endif

#endif // _WIN32

#ifdef __APPLE__
# include <mach-o/dyld.h>
# include <unistd.h>
# include <mach/machine.h>
# include <mach-o/loader.h>
# include <mach-o/fat.h>
#endif

#if defined(__linux__)
# include <elf.h>
#endif

#if USE_WINE
// avoid including Wine headers just for a few defines
# define IMAGE_FILE_MACHINE_AMD64 0x8664
# define IMAGE_FILE_MACHINE_I386 0x014c
# define IMAGE_FILE_MACHINE_POWERPC 0x01f0
# define IMAGE_FILE_MACHINE_ARM 0x01c0
# define IMAGE_FILE_MACHINE_ARM64 0xaa64
# define IMAGE_FILE_DLL 0x2000
#endif // USE_WINE

#include <sstream>
#include <cstring>

namespace vst {

CpuArch getHostCpuArchitecture(){
#if defined(__i386__) || defined(_M_IX86)
    return CpuArch::i386;
#elif defined(__x86_64__) || defined(_M_X64)
    return CpuArch::amd64;
#elif defined(__arm__) || defined(_M_ARM)
    return CpuArch::arm;
#elif defined(__aarch64__)
    return CpuArch::aarch64;
#elif defined(__ppc__)
    return CpuArch::ppc;
#elif defined(__ppc64__)
    return CpuArch::ppc64;
#else
    return CpuArch::unknown;
#endif
}

const char * cpuArchToString(CpuArch arch){
    switch (arch){
    case CpuArch::i386:
        return "i386";
    case CpuArch::amd64:
        return "amd64";
    case CpuArch::arm:
        return "arm";
    case CpuArch::aarch64:
        return "aarch64";
    case CpuArch::ppc:
        return "ppc";
    case CpuArch::ppc64:
        return "ppc64";
#ifndef _WIN32
    case CpuArch::pe_i386:
        return "pe_i386";
    case CpuArch::pe_amd64:
        return "pe_amd64";
#endif
    default:
        return "unknown";
    }
}

static std::unordered_map<std::string, CpuArch> gCpuArchMap = {
    { "i386", CpuArch::i386 },
    { "amd64", CpuArch::amd64 },
    { "arm", CpuArch::arm },
    { "aarch64", CpuArch::aarch64 },
    { "ppc", CpuArch::ppc },
    { "ppc64", CpuArch::ppc64 },
#ifndef _WIN32
    // PE executables (for Wine support)
    { "pe_i386", CpuArch::pe_i386 },
    { "pe_amd64", CpuArch::pe_amd64 },
#endif
};

CpuArch cpuArchFromString(const std::string &name){
    auto it = gCpuArchMap.find(name);
    if (it != gCpuArchMap.end()){
        return it->second;
    } else {
        return CpuArch::unknown;
    }
}

template<typename T>
static void swap_bytes(T& i){
    const auto n = sizeof(T);
    char a[n];
    char b[n];
    memcpy(a, &i, n);
    for (int i = 0, j = n-1; i < n; ++i, --j){
        b[i] = a[j];
    }
    memcpy(&i, b, n);
}

namespace detail {
#if defined(_WIN32) || USE_WINE
CpuArch readPE(vst::File& file){
    // read PE header
    // note: we don't have to worry about byte order (always LE)
    const uint16_t dos_signature = 0x5A4D;
    const char pe_signature[] = { 'P', 'E', 0, 0 };
    const auto header_size = 24; // PE signature + COFF header
    char data[1024]; // should be large enough for DOS stub
    file.read(data, sizeof(data));
    int nbytes = file.gcount();
    // check DOS signature
    if (nbytes > sizeof(dos_signature) && !memcmp(data, &dos_signature, sizeof(dos_signature))){
        int32_t offset;
        // get the file offset to the PE signature
        memcpy(&offset, &data[0x3C], sizeof(offset));
        if (offset < (sizeof(data) - header_size)){
            const char *header = data + offset;
            if (!memcmp(header, pe_signature, sizeof(pe_signature))){
                header += sizeof(pe_signature);
                // check if it is a DLL
                uint16_t flags;
                memcpy(&flags, &header[18], sizeof(flags));
                if (!(flags & IMAGE_FILE_DLL)){
                    throw Error(Error::ModuleError, "not a DLL");
                }
                // get CPU architecture
                uint16_t arch;
                memcpy(&arch, &header[0], sizeof(arch));
                switch (arch){
                case IMAGE_FILE_MACHINE_AMD64:
                #ifdef _WIN32
                    return CpuArch::amd64;
                #else
                    return CpuArch::pe_amd64;
                #endif
                case IMAGE_FILE_MACHINE_I386:
                #ifdef _WIN32
                    return CpuArch::i386;
                #else
                    return CpuArch::pe_i386;
                #endif
                case IMAGE_FILE_MACHINE_POWERPC:
                    return CpuArch::ppc;
                case IMAGE_FILE_MACHINE_ARM:
                    return CpuArch::arm;
                case IMAGE_FILE_MACHINE_ARM64:
                    return CpuArch::aarch64;
                default:
                    return CpuArch::unknown;
                }
            } else {
                throw Error(Error::ModuleError, "bad PE signature");
            }
        } else {
            throw Error(Error::ModuleError, "DOS stub too large");
        }
    } else {
    #if USE_WINE
        throw Error(Error::NoError); // HACK!
    #else
        throw Error(Error::ModuleError, "not a DLL");
    #endif
    }
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__) // Linux, OpenBSD, FreeBSD, etc. (TODO handle Android?)
CpuArch readELF(vst::File& file){
    // read ELF header
    // check magic number
    char data[64]; // ELF header size
    if (file.read(data, sizeof(data)) && !memcmp(data, ELFMAG, SELFMAG)){
        char endian = data[0x05];
        int byteorder;
        if (endian == ELFDATA2LSB){
            byteorder = LITTLE_ENDIAN;
        } else if (endian == ELFDATA2MSB){
            byteorder = BIG_ENDIAN;
        } else {
            throw Error(Error::ModuleError, "invalid data encoding in ELF header");
        }
        // check file type
        uint16_t filetype;
        memcpy(&filetype, &data[0x10], sizeof(filetype));
        if (BYTE_ORDER != byteorder){
            swap_bytes(filetype);
        }
        // check if it is a shared object
        if (filetype != ET_DYN){
            throw Error(Error::ModuleError, "not a shared object");
        }
        // read CPU architecture
        uint16_t arch;
        memcpy(&arch, &data[0x12], sizeof(arch));
        if (BYTE_ORDER != byteorder){
            swap_bytes(arch);
        }
        switch (arch){
        case EM_386:
            return CpuArch::i386;
        case EM_X86_64:
            return CpuArch::amd64;
        case EM_PPC:
            return CpuArch::ppc;
        case EM_PPC64:
            return CpuArch::ppc64;
        case EM_ARM:
            return CpuArch::arm;
        case EM_AARCH64:
            return CpuArch::aarch64;
        default:
            return CpuArch::unknown;
        }
    } else {
        throw Error(Error::ModuleError, "not a shared object");
    }
}
#endif

#ifdef __APPLE__ // macOS (TODO handle iOS?)
std::vector<CpuArch> readMach(vst::File& file){
    // read Mach-O header
    auto read_uint32 = [](std::fstream& f, bool swap){
        uint32_t i;
        if (!f.read((char *)&i, sizeof(i))){
            throw Error(Error::ModuleError, "end of file reached");
        }
        if (swap){
            swap_bytes(i);
        }
        return i;
    };

    auto getCpuArch = [](cpu_type_t arch){
        switch (arch){
        // case CPU_TYPE_I386:
        case CPU_TYPE_X86:
            return CpuArch::i386;
        case CPU_TYPE_X86_64:
            return CpuArch::amd64;
        case CPU_TYPE_ARM:
            return CpuArch::arm;
        case CPU_TYPE_ARM64:
            return CpuArch::aarch64;
        case CPU_TYPE_POWERPC:
            return CpuArch::ppc;
        case CPU_TYPE_POWERPC64:
            return CpuArch::ppc64;
        default:
            return CpuArch::unknown;
        }
    };

    auto readMachHeader = [&](std::fstream& f, bool swap, bool wide){
        LOG_DEBUG("reading mach-o header");
        cpu_type_t cputype = read_uint32(f, swap);
        uint32_t cpusubtype = read_uint32(f, swap); // ignored
        uint32_t filetype = read_uint32(f, swap);
        // check if it is a dylib or Mach-bundle
        if (filetype != MH_DYLIB && filetype != MH_BUNDLE){
            throw Error(Error::ModuleError, "not a plugin");
        }
        return getCpuArch(cputype);
    };

    auto readFatArchive = [&](std::fstream& f, bool swap, bool wide){
        LOG_DEBUG("reading fat archive");
        std::vector<CpuArch> archs;
        auto count = read_uint32(f, swap);
        for (auto i = 0; i < count; ++i){
            // fat_arch is 20 bytes and fat_arch_64 is 32 bytes
            // read CPU type
            cpu_type_t arch = read_uint32(f, swap);
            // the archive should contain only plugins, so we don't
            // catch exepctions thrown by readMachHeader()
            archs.push_back(getCpuArch(arch));
            // skip remaining bytes. LATER also check file type.
            if (wide){
                char dummy[28];
                f.read(dummy, sizeof(dummy));
            } else {
                char dummy[16];
                f.read(dummy, sizeof(dummy));
            }
        }
        return archs;
    };

    uint32_t magic = 0;
    file.read((char *)&magic, sizeof(magic));

    // *_CIGAM tells us to swap endianess
    switch (magic){
    case MH_MAGIC:
        return { readMachHeader(file, false, false) };
    case MH_CIGAM:
        return { readMachHeader(file, true, false) };
#ifdef MH_MAGIC_64
    case MH_MAGIC_64:
        return { readMachHeader(file, false, true) };
    case MH_CIGAM_64:
        return { readMachHeader(file, true, true) };
#endif
    case FAT_MAGIC:
        return readFatArchive(file, false, false);
    case FAT_CIGAM:
        return readFatArchive(file, true, false);
#ifdef FAT_MAGIC_64
    case FAT_MAGIC_64:
        return readFatArchive(file, false, true);
    case FAT_CIGAM_64:
        return readFatArchive(file, true, true);
#endif
    default:
        return {};
    }
}
#endif

// Check a file path or bundle for contained CPU architectures
// If 'path' is a file, we throw an exception if it is not a library,
// but if 'path' is a bundle (= directory), we ignore any non-library files
// in the 'Contents' subfolder (so the resulting list might be empty).
// However, we always throw exceptions when we encounter errors.
std::vector<CpuArch> getCpuArchitectures(const std::string& path, bool bundle){
    std::vector<CpuArch> results;
    if (isDirectory(path)){
        // plugin bundle: iterate over Contents/* recursively
        vst::search(path + "/Contents", [&](const std::string& resPath){
            auto res = getCpuArchitectures(resPath, true); // bundle!
            results.insert(results.end(), res.begin(), res.end());
        }, false); // don't filter
    } else {
        // ignore files that are not plugins
        auto hasExtension = [](const std::string& path){
        #ifdef __APPLE__
            // on Apple, the actual dylib inside the bundle doesn't have
            // a file extension, so we must check for empty extensions!
            auto e1 = fileExtension(path);
            if (e1.empty()){
                return true; // accept empty extension
            }
        #else
            auto e1 = fileExtension(path);
        #endif
            for (auto& e2 : getPluginExtensions()){
                if (e1 == e2){
                    return true;
                }
            }
            return false;
        };
        if (hasExtension(path)){
            vst::File file(path);
            if (file.is_open()){
                try {
                #if defined(_WIN32) // Windows
                    results.push_back(readPE(file));
                #elif defined(__APPLE__)
                    auto archs = readMach(file);
                    results.insert(results.end(), archs.begin(), archs.end());
                #else
                    results.push_back(readELF(file));
                #endif
                } catch (const Error& e) {
                #if !defined(_WIN32) && USE_WINE
                    // rewind file!
                    file.clear();
                    file.seekg(0);
                    try {
                        // try to read as PE
                        results.push_back(readPE(file));
                    } catch (const Error& e2) {
                        Error err;
                        if (e2.code() == Error::NoError){
                            // not a PE, keep original error
                            err = e;
                        } else {
                            // bad PE
                            err = e2;
                        }
                        if (!bundle){
                            throw err;
                        } else {
                            LOG_ERROR(path << ": " << err.what());
                        }
                    }
                #else
                    if (!bundle){
                        throw; // rethrow
                    } else {
                        LOG_ERROR(path << ": " << e.what());
                    }
                #endif
                }
            } else {
                if (!bundle){
                    throw Error(Error::ModuleError, "couldn't open file");
                } else {
                    LOG_ERROR("couldn't open " << path);
                }
            }
        }
    }
    return results;
}
} // detail

std::vector<CpuArch> getCpuArchitectures(const std::string& path){
    auto results = detail::getCpuArchitectures(path, false);
    if (results.empty()) {
        // this code path is only reached by bundles
        throw Error(Error::ModuleError, "bundle doesn't contain any plugins");
    }
    return results;
}

void printCpuArchitectures(const std::string& path){
    auto archs = getCpuArchitectures(path);
    if (!archs.empty()){
        std::stringstream ss;
        for (auto& arch : archs){
            ss << cpuArchToString(arch) << " ";
        }
        LOG_VERBOSE("CPU architectures: " << ss.str());
    } else {
        LOG_VERBOSE("CPU architectures: none");
    }
}

} // vst
