#include "VST2Plugin.h"

#include <iostream>

VST2Plugin::VST2Plugin(void *plugin, const std::string& path)
    : VSTPlugin(path), plugin_((AEffect*)plugin)
{
    dispatch(effOpen);
    dispatch(effMainsChanged, 0, 1);
}

VST2Plugin::~VST2Plugin(){
    // dispatch(effClose, 0, 0, NULL, 0.f);
}

std::string VST2Plugin::getPluginName() const {
    char buf[256];
    buf[0] = 0;
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
    return plugin_->processReplacing != nullptr;
}

bool VST2Plugin::hasDoublePrecision() const {
    return plugin_->processDoubleReplacing != nullptr;
}

void VST2Plugin::resume(){
    dispatch(effMainsChanged, 0, 1);
}

void VST2Plugin::pause(){
    dispatch(effMainsChanged, 0, 0);
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

void VST2Plugin::setParameter(int index, float value){
    plugin_->setParameter(plugin_, index, value);
    // update();
}

float VST2Plugin::getParameter(int index) const {
    return (plugin_->getParameter)(plugin_, index);
}

std::string VST2Plugin::getParameterName(int index) const {
    char buf[256];
    buf[0] = 0;
    dispatch(effGetParamName, index, 0, buf);
    return std::string(buf);
}

std::string VST2Plugin::getParameterLabel(int index) const {
    char buf[256];
    buf[0] = 0;
    dispatch(effGetParamLabel, index, 0, buf);
    return std::string(buf);
}

std::string VST2Plugin::getParameterDisplay(int index) const {
    char buf[256];
    buf[0] = 0;
    dispatch(effGetParamDisplay, index, 0, buf);
    return std::string(buf);
}

int VST2Plugin::getNumParameters() const {
    return plugin_->numParams;
}

void VST2Plugin::setProgram(int program){
    if (program >= 0 && program < getNumPrograms()){
        dispatch(effSetProgram, 0, program);
        // update();
    } else {
        std::cout << "program number out of range!" << std::endl;
    }
}

void VST2Plugin::setProgramName(const std::string& name){
    dispatch(effSetProgramName, 0, 0, (void*)name.c_str());
}

int VST2Plugin::getProgram(){
    return dispatch(effGetProgram, 0, 0, NULL, 0.f);
}

std::string VST2Plugin::getProgramName() const {
    char buf[256];
    buf[0] = 0;
    dispatch(effGetProgramName, 0, 0, buf);
    return std::string(buf);
}

std::string VST2Plugin::getProgramNameIndexed(int index) const {
    char buf[256];
    buf[0] = 0;
    dispatch(effGetProgramNameIndexed, index, 0, buf);
    return std::string(buf);
}

int VST2Plugin::getNumPrograms() const {
    return plugin_->numPrograms;
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

bool VST2Plugin::hasFlag(VstAEffectFlags flag) const {
    return plugin_->flags & flag;
}

VstIntPtr VST2Plugin::dispatch(VstInt32 opCode,
    VstInt32 index, VstInt32 value, void *ptr, float opt) const {
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

