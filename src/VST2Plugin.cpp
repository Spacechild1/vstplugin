#include "VST2Plugin.h"

#include <iostream>
#include <fstream>

/*------------------ endianess -------------------*/
    // endianess check taken from Pure Data (d_osc.c)
#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__FreeBSD_kernel__) \
    || defined(__OpenBSD__)
#include <machine/endian.h>
#endif

#if defined(__linux__) || defined(__CYGWIN__) || defined(__GNU__) || \
    defined(ANDROID)
#include <endian.h>
#endif

#ifdef __MINGW32__
#include <sys/param.h>
#endif

#ifdef _MSC_VER
/* _MSVC lacks BYTE_ORDER and LITTLE_ENDIAN */
#define LITTLE_ENDIAN 0x0001
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#if !defined(BYTE_ORDER) || !defined(LITTLE_ENDIAN)
#error No byte order defined
#endif

union union32 {
    float un_float;
    int32_t un_int32;
    char un_bytes[4];
};

// big endian (de)serialization routines (.FXP and .FXB files store all their data in big endian)
static void int32_to_bytes(int32_t i, char *bytes){
    union32 u;
    u.un_int32 = i;
#if BYTE_ORDER == LITTLE_ENDIAN
        // swap endianess
    bytes[0] = u.un_bytes[3];
    bytes[1] = u.un_bytes[2];
    bytes[2] = u.un_bytes[1];
    bytes[3] = u.un_bytes[0];
#else
    bytes[0] = u.un_bytes[0];
    bytes[1] = u.un_bytes[1];
    bytes[2] = u.un_bytes[1];
    bytes[3] = u.un_bytes[3];
#endif
}

static void float_to_bytes(float f, char *bytes){
    union32 u;
    u.un_float = f;
#if BYTE_ORDER == LITTLE_ENDIAN
        // swap endianess
    bytes[0] = u.un_bytes[3];
    bytes[1] = u.un_bytes[2];
    bytes[2] = u.un_bytes[1];
    bytes[3] = u.un_bytes[0];
#else
    bytes[0] = u.un_bytes[0];
    bytes[1] = u.un_bytes[1];
    bytes[2] = u.un_bytes[1];
    bytes[3] = u.un_bytes[3];
#endif
}

static int32_t bytes_to_int32(const char *bytes){
    union32 u;
#if BYTE_ORDER == LITTLE_ENDIAN
        // swap endianess
    u.un_bytes[3] = bytes[0];
    u.un_bytes[2] = bytes[1];
    u.un_bytes[1] = bytes[2];
    u.un_bytes[0] = bytes[3];
#else
    u.un_bytes[0] = bytes[0];
    u.un_bytes[1] = bytes[1];
    u.un_bytes[2] = bytes[2];
    u.un_bytes[3] = bytes[3];
#endif
    return u.un_int32;
}

static float bytes_to_float(const char *bytes){
    union32 u;
#if BYTE_ORDER == LITTLE_ENDIAN
        // swap endianess
    u.un_bytes[3] = bytes[0];
    u.un_bytes[2] = bytes[1];
    u.un_bytes[1] = bytes[2];
    u.un_bytes[0] = bytes[3];
#else
    u.un_bytes[0] = bytes[0];
    u.un_bytes[1] = bytes[1];
    u.un_bytes[2] = bytes[2];
    u.un_bytes[3] = bytes[3];
#endif
    return u.un_float;
}

/*----------- fxProgram and fxBank file structures (see vstfxstore.h)-------------*/
const size_t fxProgramHeaderSize = 56;  // 7 * VstInt32 + 28 character program name
const size_t fxBankHeaderSize = 156;    // 8 * VstInt32 + 124 empty characters
// replacements for multicharacter constants
#define cMagic (bytes_to_int32("CcnK"))
#define fMagic (bytes_to_int32("FxCk"))
#define bankMagic (bytes_to_int32("FxBk"))
#define chunkPresetMagic (bytes_to_int32("FPCh"))
#define chunkBankMagic (bytes_to_int32("FBCh"))

/*/////////////////////// VST2Plugin /////////////////////////////*/

VST2Plugin::VST2Plugin(void *plugin, const std::string& path)
    : plugin_((AEffect*)plugin), path_(path)
{
    dispatch(effOpen);
    dispatch(effMainsChanged, 0, 1);
}

VST2Plugin::~VST2Plugin(){
    dispatch(effClose);
}

std::string VST2Plugin::getPluginName() const {
    char buf[256] = {0};
    dispatch(effGetEffectName, 0, 0, buf);
    std::string name(buf);
    if (name.size()){
        return name;
    } else {
        return getBaseName();
    }
}

int VST2Plugin::getPluginVersion() const {
    return plugin_->version;
}

int VST2Plugin::getPluginUniqueID() const {
    return plugin_->uniqueID;
}

void VST2Plugin::process(float **inputs,
    float **outputs, VstInt32 sampleFrames){
    if (plugin_->processReplacing){
        (plugin_->processReplacing)(plugin_, inputs, outputs, sampleFrames);
    }
}

void VST2Plugin::processDouble(double **inputs,
    double **outputs, VstInt32 sampleFrames){
    if (plugin_->processDoubleReplacing){
        (plugin_->processDoubleReplacing)(plugin_, inputs, outputs, sampleFrames);
    }
}

bool VST2Plugin::hasSinglePrecision() const {
    return plugin_->flags & effFlagsCanReplacing;
}

bool VST2Plugin::hasDoublePrecision() const {
    return plugin_->flags & effFlagsCanDoubleReplacing;
}

void VST2Plugin::suspend(){
    dispatch(effMainsChanged, 0, 0);
}

void VST2Plugin::resume(){
    dispatch(effMainsChanged, 0, 1);
}

void VST2Plugin::setSampleRate(float sr){
    dispatch(effSetSampleRate, 0, 0, NULL, sr);
}

void VST2Plugin::setBlockSize(int n){
    dispatch(effSetBlockSize, 0, n);
}

int VST2Plugin::getNumInputs() const {
    return plugin_->numInputs;
}

int VST2Plugin::getNumOutputs() const {
    return plugin_->numOutputs;
}

bool VST2Plugin::isSynth() const {
    return hasFlag(effFlagsIsSynth);
}

bool VST2Plugin::hasTail() const {
    return !hasFlag(effFlagsNoSoundInStop);
}

int VST2Plugin::getTailSize() const {
    return dispatch(effGetTailSize);
}

bool VST2Plugin::hasBypass() const {
    return canDo("bypass");
}

void VST2Plugin::setBypass(bool bypass){
    dispatch(effSetBypass, 0, bypass);
}

void VST2Plugin::setParameter(int index, float value){
    plugin_->setParameter(plugin_, index, value);
}

float VST2Plugin::getParameter(int index) const {
    return (plugin_->getParameter)(plugin_, index);
}

std::string VST2Plugin::getParameterName(int index) const {
    char buf[256] = {0};
    dispatch(effGetParamName, index, 0, buf);
    return std::string(buf);
}

std::string VST2Plugin::getParameterLabel(int index) const {
    char buf[256] = {0};
    dispatch(effGetParamLabel, index, 0, buf);
    return std::string(buf);
}

std::string VST2Plugin::getParameterDisplay(int index) const {
    char buf[256] = {0};
    dispatch(effGetParamDisplay, index, 0, buf);
    return std::string(buf);
}

int VST2Plugin::getNumParameters() const {
    return plugin_->numParams;
}

void VST2Plugin::setProgram(int program){
    if (program >= 0 && program < getNumPrograms()){
        dispatch(effBeginSetProgram);
        dispatch(effSetProgram, 0, program);
        dispatch(effEndSetProgram);
            // update();
    } else {
        std::cout << "program number out of range!" << std::endl;
    }
}

void VST2Plugin::setProgramName(const std::string& name){
    dispatch(effSetProgramName, 0, 0, (void*)name.c_str());
}

int VST2Plugin::getProgram() const {
    return dispatch(effGetProgram, 0, 0, NULL, 0.f);
}

std::string VST2Plugin::getProgramName() const {
    char buf[256] = {0};
    dispatch(effGetProgramName, 0, 0, buf);
    return std::string(buf);
}

std::string VST2Plugin::getProgramNameIndexed(int index) const {
    char buf[256] = {0};
    dispatch(effGetProgramNameIndexed, index, 0, buf);
    return std::string(buf);
}

int VST2Plugin::getNumPrograms() const {
    return plugin_->numPrograms;
}

bool VST2Plugin::hasChunkData() const {
    return hasFlag(effFlagsProgramChunks);
}

void VST2Plugin::setProgramChunkData(const void *data, size_t size){
    dispatch(effSetChunk, true, size, const_cast<void *>(data));
}

void VST2Plugin::getProgramChunkData(void **data, size_t *size) const {
    *size = dispatch(effGetChunk, true, 0, data);
}

void VST2Plugin::setBankChunkData(const void *data, size_t size){
    dispatch(effSetChunk, false, size, const_cast<void *>(data));
}

void VST2Plugin::getBankChunkData(void **data, size_t *size) const {
    *size = dispatch(effGetChunk, false, 0, data);
}

bool VST2Plugin::readProgramFile(const std::string& path){
    std::ifstream file(path, std::ios_base::binary);
    if (!file.is_open()){
        std::cout << "VST2Plugin::readProgramFile: couldn't open file " << path << std::endl;
        return false;
    }
    file.seekg(0, std::ios_base::end);
    std::string buffer;
    buffer.resize(file.tellg());
    file.seekg(0, std::ios_base::beg);
    file.read(&buffer[0], buffer.size());
    return readProgramData(buffer);
}

bool VST2Plugin::readProgramData(const char *data, size_t size){
    if (size < fxProgramHeaderSize){  // see vstfxstore.h
        std::cout << "fxProgram: bad header size" << std::endl;
        return false;
    }
    const VstInt32 chunkMagic = bytes_to_int32(data);
    const VstInt32 byteSize = bytes_to_int32(data+4);
        // byteSize excludes 'chunkMagic' and 'byteSize' fields
    const size_t totalSize = byteSize + 8;
    const VstInt32 fxMagic = bytes_to_int32(data+8);
    // const VstInt32 version = bytes_to_int32(data+12);
    // const VstInt32 fxID = bytes_to_int32(data+16);
    // const VstInt32 fxVersion = bytes_to_int32(data+20);
    const VstInt32 numParams = bytes_to_int32(data+24);
    const char *prgName = data+28;
    const char *prgData = data + fxProgramHeaderSize;
    if (chunkMagic != cMagic){
        std::cout << "fxProgram: bad format" << std::endl;
        return false;
    }
    if (totalSize > size){
        std::cout << "fxProgram: too little data" << std::endl;
        return false;
    }

    if (fxMagic == fMagic){ // list of parameters
        if (hasChunkData()){
            std::cout << "fxProgram: plugin expects chunk data" << std::endl;
            return false;
        }
        if (numParams * sizeof(float) > totalSize - fxProgramHeaderSize){
            std::cout << "fxProgram: byte size doesn't match number of parameters" << std::endl;
            return false;
        }
        setProgramName(prgName);
        for (int i = 0; i < numParams; ++i){
            setParameter(i, bytes_to_float(prgData));
            prgData += sizeof(float);
        }
    } else if (fxMagic == chunkPresetMagic){ // chunk data
        if (!hasChunkData()){
            std::cout << "fxProgram: plugin doesn't expect chunk data" << std::endl;
            return false;
        }
        const size_t chunkSize = bytes_to_int32(prgData);
        if (chunkSize != totalSize - fxProgramHeaderSize - 4){
            std::cout << "fxProgram: wrong chunk size" << std::endl;
            return false;
        }
        setProgramName(prgName);
        setProgramChunkData(prgData + 4, chunkSize);
    } else {
        std::cout << "fxProgram: bad format" << std::endl;
        return false;
    }
    return true;
}

void VST2Plugin::writeProgramFile(const std::string& path){
    std::ofstream file(path, std::ios_base::binary | std::ios_base::trunc);
    if (!file.is_open()){
        std::cout << "VST2Plugin::writeProgramFile: couldn't create file " << path << std::endl;
        return;
    }
    std::string buffer;
    writeProgramData(buffer);
    file.write(buffer.data(), buffer.size());
}

void VST2Plugin::writeProgramData(std::string& buffer){
    VstInt32 header[7];
    header[0] = cMagic;
    header[3] = 1; // format version (always 1)
    header[4] = getPluginUniqueID();
    header[5] = getPluginVersion();
    header[6] = getNumParameters();

    char prgName[28];
    strncpy(prgName, getProgramName().c_str(), 27);
    prgName[27] = '\0';

    if (!hasChunkData()){
            // parameters
        header[2] = fMagic;
        const int nparams = header[6];
        const size_t totalSize = fxProgramHeaderSize + nparams * sizeof(float);
        header[1] = totalSize - 8; // byte size: totalSize - 'chunkMagic' - 'byteSize'
        buffer.resize(totalSize);
        char *bufptr = &buffer[0];
            // serialize header
        for (int i = 0; i < 7; ++i){
            int32_to_bytes(header[i], bufptr);
            bufptr += 4;
        }
            // serialize program name
        memcpy(bufptr, prgName, 28);
        bufptr += 28;
            // serialize parameters
        for (int i = 0; i < nparams; ++i){
            float_to_bytes(getParameter(i), bufptr);
            bufptr += sizeof(float);
        }
    } else {
            // chunk data
        header[2] = chunkPresetMagic;
        char *chunkData = nullptr;
        size_t chunkSize = 0;
        getProgramChunkData((void **)&chunkData, &chunkSize);
        if (!(chunkData && chunkSize)){
                // shouldn't happen...
            std::cout << "fxProgram bug: couldn't get chunk data" << std::endl;
            return;
        }
            // totalSize: header size + 'size' field + actual chunk data
        const size_t totalSize = fxProgramHeaderSize + 4 + chunkSize;
            // byte size: totalSize - 'chunkMagic' - 'byteSize'
        header[1] = totalSize - 8;
        buffer.resize(totalSize);
        char *bufptr = &buffer[0];
            // serialize header
        for (int i = 0; i < 7; ++i){
            int32_to_bytes(header[i], bufptr);
            bufptr += 4;
        }
            // serialize program name
        memcpy(bufptr, prgName, 28);
        bufptr += 28;
            // serialize chunk data
        int32_to_bytes(chunkSize, bufptr); // size
        memcpy(bufptr + 4, chunkData, chunkSize); // data
    }
}

bool VST2Plugin::readBankFile(const std::string& path){
    std::ifstream file(path, std::ios_base::binary);
    if (!file.is_open()){
        std::cout << "VST2Plugin::readBankFile: couldn't open file " << path << std::endl;
        return false;
    }
    file.seekg(0, std::ios_base::end);
    std::string buffer;
    buffer.resize(file.tellg());
    file.seekg(0, std::ios_base::beg);
    file.read(&buffer[0], buffer.size());
    return readBankData(buffer);
}

bool VST2Plugin::readBankData(const char *data, size_t size){
    if (size < fxBankHeaderSize){  // see vstfxstore.h
        std::cout << "fxBank: bad header size" << std::endl;
        return false;
    }
    const VstInt32 chunkMagic = bytes_to_int32(data);
    const VstInt32 byteSize = bytes_to_int32(data+4);
        // byteSize excludes 'chunkMagic' and 'byteSize' fields
    const size_t totalSize = byteSize + 8;
    const VstInt32 fxMagic = bytes_to_int32(data+8);
    // const VstInt32 version = bytes_to_int32(data+12);
    // const VstInt32 fxID = bytes_to_int32(data+16);
    // const VstInt32 fxVersion = bytes_to_int32(data+20);
    const VstInt32 numPrograms = bytes_to_int32(data+24);
    const VstInt32 currentProgram = bytes_to_int32(data + 28);
    const char *bankData = data + fxBankHeaderSize;
    if (chunkMagic != cMagic){
        std::cout << "fxBank: bad format" << std::endl;
        return false;
    }
    if (totalSize > size){
        std::cout << "fxBank: too little data" << std::endl;
        return false;
    }

    if (fxMagic == bankMagic){ // list of parameters
        if (hasChunkData()){
            std::cout << "fxBank: plugin expects chunk data" << std::endl;
            return false;
        }
        const size_t programSize = fxProgramHeaderSize + getNumParameters() * sizeof(float);
        if (numPrograms * programSize > totalSize - fxBankHeaderSize){
            std::cout << "fxBank: byte size doesn't match number of programs" << std::endl;
            return false;
        }
        for (int i = 0; i < numPrograms; ++i){
            setProgram(i);
            readProgramData(bankData, programSize);
            bankData += programSize;
        }
        setProgram(currentProgram);
    } else if (fxMagic == chunkBankMagic){ // chunk data
        if (!hasChunkData()){
            std::cout << "fxBank: plugin doesn't expect chunk data" << std::endl;
            return false;
        }
        const size_t chunkSize = bytes_to_int32(bankData);
        if (chunkSize != totalSize - fxBankHeaderSize - 4){
            std::cout << "fxBank: wrong chunk size" << std::endl;
            return false;
        }
        setBankChunkData(bankData + 4, chunkSize);
    } else {
        std::cout << "fxBank: bad format" << std::endl;
        return false;
    }
    return true;
}

void VST2Plugin::writeBankFile(const std::string& path){
    std::ofstream file(path, std::ios_base::binary | std::ios_base::trunc);
    if (!file.is_open()){
        std::cout << "VST2Plugin::writeBankFile: couldn't create file " << path << std::endl;
        return;
    }
    std::string buffer;
    writeBankData(buffer);
    file.write(buffer.data(), buffer.size());
}

void VST2Plugin::writeBankData(std::string& buffer){
    VstInt32 header[8];
    header[0] = cMagic;
    header[3] = 1; // format version (always 1)
    header[4] = getPluginUniqueID();
    header[5] = getPluginVersion();
    header[6] = getNumPrograms();
    header[7] = getProgram();

    if (!hasChunkData()){
            // programs
        header[2] = bankMagic;
        const int nprograms = header[6];
        const size_t programSize = fxProgramHeaderSize + getNumParameters() * sizeof(float);
        const size_t totalSize = fxBankHeaderSize + nprograms * programSize;
        header[1] = totalSize - 8; // byte size: totalSize - 'chunkMagic' - 'byteSize'
        buffer.resize(totalSize);
        char *bufptr = &buffer[0];
            // serialize header
        for (int i = 0; i < 8; ++i){
            int32_to_bytes(header[i], bufptr);
            bufptr += 4;
        }
        bufptr = &buffer[fxBankHeaderSize];
            // serialize programs
        std::string progData; // use intermediate buffer so we can reuse writeProgramData
        for (int i = 0; i < nprograms; ++i){
            setProgram(i);
            writeProgramData(progData);
            if (progData.size() != programSize){
                    // shouldn't happen...
                std::cout << "fxBank bug: wrong program data size" << std::endl;
                buffer.clear();
                return;
            }
            memcpy(bufptr, progData.data(), progData.size());
            bufptr += programSize;
        }
        setProgram(header[7]); // restore current program
    } else {
            // chunk data
        header[2] = chunkBankMagic;
        char *chunkData = nullptr;
        size_t chunkSize = 0;
        getBankChunkData((void **)&chunkData, &chunkSize);
        if (!(chunkData && chunkSize)){
                // shouldn't happen...
            std::cout << "fxBank bug: couldn't get chunk data" << std::endl;
            return;
        }
            // totalSize: header size + 'size' field + actual chunk data
        size_t totalSize = fxBankHeaderSize + 4 + chunkSize;
            // byte size: totalSize - 'chunkMagic' - 'byteSize'
        header[1] = totalSize - 8;
        buffer.resize(totalSize);
        char *bufptr = &buffer[0];
            // serialize header
        for (int i = 0; i < 8; ++i){
            int32_to_bytes(header[i], bufptr);
            bufptr += 4;
        }
        bufptr = &buffer[fxBankHeaderSize];
            // serialize chunk data
        int32_to_bytes(chunkSize, bufptr); // size
        memcpy(bufptr + 4, chunkData, chunkSize); // data
    }
}

bool VST2Plugin::hasEditor() const {
    return hasFlag(effFlagsHasEditor);
}

void VST2Plugin::openEditor(void * window){
    dispatch(effEditOpen, 0, 0, window);
}

void VST2Plugin::closeEditor(){
    dispatch(effEditClose);
}

void VST2Plugin::getEditorRect(int &left, int &top, int &right, int &bottom) const {
    ERect* erc = nullptr;
    dispatch(effEditGetRect, 0, 0, &erc);
    if (erc){
        left = erc->left;
        top = erc->top;
        right = erc->right;
        bottom = erc->bottom;
    } else {
        std::cerr << "VST2Plugin::getEditorRect: bug!" << std::endl;
    }
}

// private
std::string VST2Plugin::getBaseName() const {
    auto sep = path_.find_last_of("\\/");
    auto dot = path_.find_last_of('.');
    if (sep == std::string::npos){
        sep = -1;
    }
    if (dot == std::string::npos){
        dot = path_.size();
    }
    return path_.substr(sep + 1, dot - sep - 1);
}

bool VST2Plugin::hasFlag(VstAEffectFlags flag) const {
    return plugin_->flags & flag;
}

bool VST2Plugin::canDo(const char *what) const {
    return dispatch(effCanDo, 0, 0, (void *)what) > 0;
}

VstIntPtr VST2Plugin::dispatch(VstInt32 opCode,
    VstInt32 index, VstIntPtr value, void *ptr, float opt) const {
    return (plugin_->dispatcher)(plugin_, opCode, index, value, ptr, opt);
}

// Main host callback
VstIntPtr VSTCALLBACK VST2Plugin::hostCallback(AEffect *plugin, VstInt32 opcode,
    VstInt32 index, VstIntPtr value, void *ptr, float opt){
        // std::cout << "plugin requested opcode " << opcode << std::endl;
    switch(opcode) {
    case audioMasterAutomate:
        std::cout << "opcode: audioMasterAutomate" << std::endl;
        break;
    case audioMasterVersion:
        std::cout << "opcode: audioMasterVersion" << std::endl;
        return 2400;
    case audioMasterCurrentId:
        std::cout << "opcode: audioMasterCurrentId" << std::endl;
        break;
    case audioMasterIdle:
        std::cout << "opcode: audioMasterIdle" << std::endl;
        plugin->dispatcher(plugin, effEditIdle, 0, 0, NULL, 0.f);
        break;
    case audioMasterGetTime:
            // std::cout << "opcode: audioMasterGetTime" << std::endl;
        break;
    case audioMasterProcessEvents:
        std::cout << "opcode: audioMasterProcessEvents" << std::endl;
        break;
    case audioMasterIOChanged:
        std::cout << "opcode: audioMasterIOChanged" << std::endl;
        break;
    case audioMasterSizeWindow:
        std::cout << "opcode: audioMasterSizeWindow" << std::endl;
        break;
    case audioMasterGetSampleRate:
        std::cout << "opcode: audioMasterGetSampleRate" << std::endl;
        break;
    case audioMasterGetBlockSize:
        std::cout << "opcode: audioMasterGetBlockSize" << std::endl;
        break;
    case audioMasterGetInputLatency:
        std::cout << "opcode: audioMasterGetInputLatency" << std::endl;
        break;
    case audioMasterGetOutputLatency:
        std::cout << "opcode: audioMasterGetOutputLatency" << std::endl;
        break;
    case audioMasterGetCurrentProcessLevel:
        std::cout << "opcode: audioMasterGetCurrentProcessLevel" << std::endl;
        break;
    case audioMasterGetAutomationState:
        std::cout << "opcode: audioMasterGetAutomationState" << std::endl;
        break;
    case audioMasterGetVendorString:
    case audioMasterGetProductString:
    case audioMasterGetVendorVersion:
    case audioMasterVendorSpecific:
        std::cout << "opcode: vendor info" << std::endl;
        break;
    case audioMasterCanDo:
        std::cout << "opcode: audioMasterCanDo " << (const char*)ptr << std::endl;
        break;
    case audioMasterGetLanguage:
        std::cout << "opcode: audioMasterGetLanguage" << std::endl;
        break;
    case audioMasterGetDirectory:
        std::cout << "opcode: audioMasterGetDirectory" << std::endl;
        break;
    case audioMasterUpdateDisplay:
        std::cout << "opcode: audioMasterUpdateDisplay" << std::endl;
        break;
    case audioMasterBeginEdit:
        std::cout << "opcode: audioMasterBeginEdit" << std::endl;
        break;
    case audioMasterEndEdit:
        std::cout << "opcode: audioMasterEndEdit" << std::endl;
        break;
    case audioMasterOpenFileSelector:
        std::cout << "opcode: audioMasterOpenFileSelector" << std::endl;
        break;
    case audioMasterCloseFileSelector:
        std::cout << "opcode: audioMasterCloseFileSelector" << std::endl;
        break;
    default:
        std::cout << "plugin requested unknown/deprecated opcode " << opcode << std::endl;
        return 0;
    }
    return 0; // ?
}
