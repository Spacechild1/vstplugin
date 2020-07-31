#include "Bridge.h"
#include "Utility.h"

namespace vst {

/*/////////////////////// PluginClient /////////////////////////////*/

PluginClient::PluginClient(IFactory::const_ptr f, PluginInfo::const_ptr desc)
    : factory_(std::move(f)), info_(std::move(desc))
{

}

PluginClient::~PluginClient(){

}

void PluginClient::setupProcessing(double sampleRate, int maxBlockSize, ProcessPrecision precision){

}

void PluginClient::process(ProcessData<float>& data){

}

void PluginClient::process(ProcessData<double>& data){

}

void PluginClient::suspend(){

}

void PluginClient::resume(){

}

void PluginClient::setBypass(Bypass state){

}

void PluginClient::setNumSpeakers(int in, int out, int auxin, int auxout){

}

void PluginClient::setTempoBPM(double tempo){
    if (tempo > 0) {

    } else {
        LOG_WARNING("setTempoBPM: tempo must be greater than 0!");
    }
}

void PluginClient::setTimeSignature(int numerator, int denominator){
    if (numerator > 0 && denominator > 0){

    } else {
        LOG_WARNING("setTimeSignature: bad time signature!");
    }
}

void PluginClient::setTransportPlaying(bool play){

}

void PluginClient::setTransportRecording(bool record){

}

void PluginClient::setTransportAutomationWriting(bool writing){

}

void PluginClient::setTransportAutomationReading(bool reading){

}

void PluginClient::setTransportCycleActive(bool active){

}

void PluginClient::setTransportCycleStart(double beat){

}

void PluginClient::setTransportCycleEnd(double beat){

}

void PluginClient::setTransportPosition(double beat){

}

void PluginClient::sendMidiEvent(const MidiEvent &event){

}

void PluginClient::sendSysexEvent(const SysexEvent &event){

}

void PluginClient::setParameter(int index, float value, int sampleOffset){
    if (index >= 0 && index < numParameters()){

    } else {
        LOG_WARNING("parameter index out of range!");
    }
}

bool PluginClient::setParameter(int index, const std::string &str, int sampleOffset){
    if (index >= 0 && index < numParameters()){
        return true;
    } else {
        LOG_WARNING("parameter index out of range!");
        return false;
    }
}

float PluginClient::getParameter(int index) const {
    if (index >= 0 && index < numParameters()){
        return 0;
    } else {
        LOG_WARNING("parameter index out of range!");
        return 0;
    }
}

std::string PluginClient::getParameterString(int index) const {
    if (index >= 0 && index < numParameters()){
        return "";
    } else {
        LOG_WARNING("parameter index out of range!");
        return "";
    }
}

void PluginClient::setProgram(int program){
    if (program >= 0 && program < numPrograms()){

    } else {
        LOG_WARNING("program number out of range!");
    }
}

void PluginClient::setProgramName(const std::string& name){

}

int PluginClient::getProgram() const {
    return 0;
}

std::string PluginClient::getProgramName() const {
    return "";
}

std::string PluginClient::getProgramNameIndexed(int index) const {
    return "";
}

void PluginClient::readProgramFile(const std::string& path){
}

void PluginClient::readProgramData(const char *data, size_t size){

}

void PluginClient::writeProgramFile(const std::string& path){
}

void PluginClient::writeProgramData(std::string& buffer){

}

void PluginClient::readBankFile(const std::string& path){

}

void PluginClient::readBankData(const char *data, size_t size){

}

void PluginClient::writeBankFile(const std::string& path){

}

void PluginClient::writeBankData(std::string& buffer){

}

void PluginClient::openEditor(void * window){

}

void PluginClient::closeEditor(){

}

// VST2 only

int PluginClient::canDo(const char *what) const {
    LOG_WARNING("canDo() not supported with bit bridging");
    return 0;
}

intptr_t PluginClient::vendorSpecific(int index, intptr_t value, void *p, float opt){
    LOG_WARNING("vendorSpecific() not supported with bit bridging");
    return 0;
}

// VST3 only

void PluginClient::beginMessage(){

}

void PluginClient::addInt(const char* id, int64_t value){

}

void PluginClient::addFloat(const char* id, double value){

}

void PluginClient::addString(const char* id, const char *value){

}

void PluginClient::addString(const char* id, const std::string& value){

}

void PluginClient::addBinary(const char* id, const char *data, size_t size){

}

void PluginClient::endMessage(){

}


/*///////////////////// WindowClient ////////////////////*/

WindowClient::WindowClient(){

}

WindowClient::~WindowClient(){

}

void* WindowClient::getHandle() {
    return nullptr;
}

void WindowClient::setTitle(const std::string& title){

}

void WindowClient::open(){

}

void WindowClient::close(){

}

void WindowClient::setPos(int x, int y){

}

void WindowClient::setSize(int w, int h){

}

void WindowClient::update(){

}

} // vst
