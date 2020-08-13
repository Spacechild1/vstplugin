#include "PluginClient.h"
#include "Utility.h"

namespace vst {

/*/////////////////////// PluginClient /////////////////////////////*/

#define FORBIDDEN_METHOD(name) throw Error(Error::PluginError, "PluginClient: must not call " name "()");

#define UNSUPPORTED_METHOD(name) LOG_WARNING(name "() not supported with bit bridging");

IPlugin::ptr makeBridgedPlugin(IFactory::const_ptr factory, const std::string& name,
                               bool editor, bool sandbox)
{
    auto info = factory->findPlugin(name); // should never fail
    if (!info){
        throw Error(Error::PluginError, "couldn't find subplugin");
    }
    auto plugin = std::make_unique<PluginClient>(factory, info, sandbox);
    if (editor){
        auto window = std::make_unique<WindowClient>(*plugin);
        plugin->setWindow(std::move(window));
    }
    return std::move(plugin); // Clang bug
}

PluginClient::PluginClient(IFactory::const_ptr f, PluginInfo::const_ptr desc, bool sandbox)
    : factory_(std::move(f)), info_(std::move(desc))
{
    static std::atomic<ID> nextID{0};
    id_ = ++nextID; // atomic incremetn!

    if (sandbox){
        bridge_ = PluginBridge::create(f->arch(), *desc);
    } else {
        bridge_ = PluginBridge::getShared(f->arch());
        // TODO open plugin
    }
}

PluginClient::~PluginClient(){
    bridge_->removeUIClient(id_);
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

int PluginClient::getLatencySamples(){
    return 0;
}

void PluginClient::setListener(IPluginListener::ptr listener) {
    listener_ = listener;
    bridge_->addUIClient(id_, listener);
}

double PluginClient::getTransportPosition() const {
    return 0;
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
    FORBIDDEN_METHOD("openEditor")
}

void PluginClient::closeEditor(){
    FORBIDDEN_METHOD("closeEditor")
}

bool PluginClient::getEditorRect(int &left, int &top, int &right, int &bottom) const {
    FORBIDDEN_METHOD("getEditorRect")
}

void PluginClient::updateEditor() {
    FORBIDDEN_METHOD("updateEditor")
}

void PluginClient::checkEditorSize(int& width, int& height) const {
    FORBIDDEN_METHOD("checkEditorSize")
}

void PluginClient::resizeEditor(int width, int height) {
    FORBIDDEN_METHOD("resizeEditor")
}

bool PluginClient::canResize() const {
    FORBIDDEN_METHOD("canResize")
}

// VST2 only

int PluginClient::canDo(const char *what) const {
    UNSUPPORTED_METHOD("canDo")
    return 0;
}

intptr_t PluginClient::vendorSpecific(int index, intptr_t value, void *p, float opt){
    UNSUPPORTED_METHOD("vendorSpecific");
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

WindowClient::WindowClient(PluginClient& plugin)
    : plugin_(&plugin)
{

}

WindowClient::~WindowClient(){

}

void* WindowClient::getHandle() {
    return nullptr;
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
    // ignore
}

} // vst
