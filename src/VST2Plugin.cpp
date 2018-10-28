#include "VST2Plugin.h"

#include <iostream>

VST2Plugin::VST2Plugin(void *plugin)
    : plugin_((AEffect*)plugin){
    dispatch(effOpen, 0, 0, NULL, 0.f);
}

VST2Plugin::~VST2Plugin(){
    dispatch(effClose, 0, 0, NULL, 0.f);
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
    dispatch(effMainsChanged, 0, 1, NULL, 0.f);
}

void VST2Plugin::pause(){
    dispatch(effMainsChanged, 0, 0, NULL, 0.f);
}

void VST2Plugin::setSampleRate(float sr){
    dispatch(effSetSampleRate, 0, 0, NULL, sr);
}

void VST2Plugin::setBlockSize(int n){
    dispatch(effSetBlockSize, 0, n, NULL, 0.f);
}

int VST2Plugin::getNumInputs() const {
    return plugin_->numInputs;
}

int VST2Plugin::getNumOutputs() const {
    return plugin_->numOutputs;
}

void VST2Plugin::setParameter(int index, float value){
    plugin_->setParameter(plugin_, index, value);
}

float VST2Plugin::getParameter(int index) const {
    return (plugin_->getParameter)(plugin_, index);
}

int VST2Plugin::getNumParameters() const {
    return plugin_->numParams;
}

std::string VST2Plugin::getParameterName(int index) const {
    char buf[1000];
    buf[0] = 0;
    dispatch(effGetParamName, index, 0, buf, 0.f);
    return std::string(buf);
}

void VST2Plugin::setProgram(int program){
    if (program >= 0 && program < getNumPrograms()){
        dispatch(effSetProgram, 0, program, NULL, 0.f);
    } else {
        std::cout << "program number out of range!" << std::endl;
    }
}

int VST2Plugin::getProgram(){
    return dispatch(effGetProgram, 0, 0, NULL, 0.f);
}

int VST2Plugin::getNumPrograms() const {
    return plugin_->numPrograms;
}

std::string VST2Plugin::getProgramName() const {
    char buf[1000];
    buf[0] = 0;
    dispatch(effGetProgramName, 0, 0, buf, 0.f);
    return std::string(buf);
}

void VST2Plugin::setProgramName(const std::string& name){
    dispatch(effSetProgramName, 0, 0, (void*)name.c_str(), 0.f);
}

bool VST2Plugin::hasEditor() const {
    return hasFlag(effFlagsHasEditor);
}

void VST2Plugin::openEditor(void * window){
    dispatch(effEditOpen, 0, 0, window, 0.f);
}

void VST2Plugin::closeEditor(){
    dispatch(effEditClose, 0, 0, NULL, 0.f);
}

void VST2Plugin::getEditorRect(int &left, int &top, int &right, int &bottom) const {
    ERect* erc = nullptr;
    dispatch(effEditGetRect, 0, 0, &erc, 0.f);
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
    VstInt32 index, VstInt32 value, void *ptr, float opt){
    switch(opcode) {
    case audioMasterVersion:
      return 2400;
    case audioMasterIdle:
      plugin->dispatcher(plugin, effEditIdle, 0, 0, 0, 0);
      break;
    // Handle other opcodes here... there will be lots of them
    default:
      break;
    }
    std::cout << "plugin requested opcode " << opcode << std::endl;
    return 0; // ?
}

