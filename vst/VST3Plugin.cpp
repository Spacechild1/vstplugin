#include "VST3Plugin.h"
#include "Utility.h"

#include <cstring>

DEF_CLASS_IID (IPluginBase)
DEF_CLASS_IID (IPlugView)
// DEF_CLASS_IID (IPlugFrame)
DEF_CLASS_IID (IPluginFactory)
DEF_CLASS_IID (IPluginFactory2)
DEF_CLASS_IID (IPluginFactory3)
DEF_CLASS_IID (Vst::IComponent)
DEF_CLASS_IID (Vst::IEditController)

#if SMTG_OS_LINUX
DEF_CLASS_IID (Linux::IEventHandler)
DEF_CLASS_IID (Linux::ITimerHandler)
DEF_CLASS_IID (Linux::IRunLoop)
#endif



namespace vst {

VST3Factory::VST3Factory(const std::string& path)
    : path_(path), module_(IModule::load(path))
{
    if (!module_){
        // shouldn't happen...
        throw VSTError("couldn't load module!");
    }
    auto factoryProc = module_->getFnPtr<GetFactoryProc>("GetPluginFactory");
    if (!factoryProc){
        throw VSTError("couldn't find 'GetPluginFactory' function");
    }
    if (!module_->init()){
        throw VSTError("couldn't init module");
    }
    factory_ = IPtr<IPluginFactory>(factoryProc());
    if (!factory_){
        throw VSTError("couldn't get VST3 plug-in factory");
    }
    /// LOG_DEBUG("VST3Factory: loaded " << path);
    // map plugin names to indices
    auto numPlugins = factory_->countClasses();
    for (int i = 0; i < numPlugins; ++i){
        VSTPluginDesc desc(*this);
        desc.path = path_;
        PClassInfo ci;
        if (factory_->getClassInfo(i, &ci) == kResultTrue){
            nameMap_[ci.name] = i;
        } else {
            LOG_ERROR("couldn't get class info!");
        }
    }
    LOG_DEBUG("module contains " << numPlugins << " plugins");
}

VST3Factory::~VST3Factory(){
    if (!module_->exit()){
        // don't throw!
        LOG_ERROR("couldn't exit module");
    }
    LOG_DEBUG("freed VST3 module" << path_);
}

std::vector<std::shared_ptr<VSTPluginDesc>> VST3Factory::plugins() const {
    return plugins_;
}

int VST3Factory::numPlugins() const {
    return plugins_.size();
}

void VST3Factory::probe() {
    plugins_.clear();
    auto numPlugins = factory_->countClasses();
    for (int i = 0; i < numPlugins; ++i){
        VSTPluginDesc desc(*this);
        desc.path = path_;
        PClassInfo ci;
        if (factory_->getClassInfo(i, &ci) == kResultTrue){
            auto result = vst::probe(path_, ci.name, desc);
            nameMap_[ci.name] = i;
            desc.probeResult = result;
        } else {
            LOG_ERROR("couldn't get class info!");
            desc.probeResult = ProbeResult::error;
        }
        plugins_.push_back(std::make_shared<VSTPluginDesc>(std::move(desc)));
    }
}

std::unique_ptr<IVSTPlugin> VST3Factory::create(const std::string& name, bool unsafe) const {
    auto it = nameMap_.find(name);
    if (it == nameMap_.end()){
        return nullptr;
    }
    auto which = it->second;
    if (!unsafe){
        if (which < 0 || which >= (int)plugins_.size()){
            LOG_WARNING("VST3Factory: no plugin");
            return nullptr;
        }
        if (!plugins_[which]->valid()){
            LOG_WARNING("VST3Factory: plugin not probed successfully");
            return nullptr;
        }
    }
    try {
        return std::make_unique<VST3Plugin>(factory_, which, path_);
    } catch (const VSTError& e){
        LOG_ERROR("couldn't create plugin: " << name);
        LOG_ERROR(e.what());
        return nullptr;
    }
}

/*/////////////////////// VST3Plugin /////////////////////////////*/

template <typename T>
inline IPtr<T> createInstance (IPtr<IPluginFactory> factory, TUID iid){
    T* obj = nullptr;
    if (factory->createInstance (iid, T::iid, reinterpret_cast<void**> (&obj)) == kResultTrue){
        return owned(obj);
    } else {
        return nullptr;
    }
}

VST3Plugin::VST3Plugin(IPtr<IPluginFactory> factory, int which, const std::string& path)
    : path_(path)
{
    PClassInfo2 ci2;
    TUID uid;
    auto factory2 = FUnknownPtr<IPluginFactory2> (factory);
    if (factory2 && factory2->getClassInfo2(which, &ci2) == kResultTrue){
        memcpy(uid, ci2.cid, sizeof(TUID));
        name_ = ci2.name;
        category_ = ci2.category;
        vendor_ = ci2.vendor;
        sdkVersion_ = ci2.sdkVersion;
        // version
        // sdk version
    } else {
        Steinberg::PClassInfo ci;
        if (factory->getClassInfo(which, &ci) == kResultTrue){
            memcpy(uid, ci.cid, sizeof(TUID));
            name_ = ci.name;
            category_ = ci.category;
            sdkVersion_ = "VST 3";
        } else {
            LOG_ERROR("couldn't get class info!");
            return;
        }
    }
    if (vendor_.empty()){
        PFactoryInfo i;
        factory->getFactoryInfo(&i);
        vendor_ = i.vendor;
    }
#if 0
    if (name_.empty()){
        name_ = getBaseName();
    }
#endif
    component_ = createInstance<Vst::IComponent>(factory, uid);
    if (!component_){
        throw VSTError("couldn't create VST3 component");
    }
    LOG_DEBUG("created VST3 component");
}

VST3Plugin::~VST3Plugin(){

}

std::string VST3Plugin::getPluginName() const {
    return name_;
}

std::string VST3Plugin::getPluginVendor() const {
    return std::string{};
}

std::string VST3Plugin::getPluginCategory() const {
    return std::string{};
}

std::string VST3Plugin::getPluginVersion() const {
    return std::string{};
}

std::string VST3Plugin::getSDKVersion() const {
    return sdkVersion_;
}

int VST3Plugin::getPluginUniqueID() const {
    return 0;
}

int VST3Plugin::canDo(const char *what) const {
    return 0;
}

intptr_t VST3Plugin::vendorSpecific(int index, intptr_t value, void *ptr, float opt){
    return 0;
}

void VST3Plugin::process(const float **inputs, float **outputs, int sampleFrames){

}

void VST3Plugin::processDouble(const double **inputs, double **outputs, int sampleFrames){

}

bool VST3Plugin::hasPrecision(VSTProcessPrecision precision) const {
    return false;
}

void VST3Plugin::setPrecision(VSTProcessPrecision precision){

}

void VST3Plugin::suspend(){

}

void VST3Plugin::resume(){

}

void VST3Plugin::setSampleRate(float sr){

}

void VST3Plugin::setBlockSize(int n){

}

int VST3Plugin::getNumInputs() const {
    return 0;
}

int VST3Plugin::getNumOutputs() const {
    return 0;
}

bool VST3Plugin::isSynth() const {
    return false;
}

bool VST3Plugin::hasTail() const {
    return false;
}

int VST3Plugin::getTailSize() const {
    return 0;
}

bool VST3Plugin::hasBypass() const {
    return false;
}

void VST3Plugin::setBypass(bool bypass){

}

void VST3Plugin::setNumSpeakers(int in, int out){

}

void VST3Plugin::setTempoBPM(double tempo){

}

void VST3Plugin::setTimeSignature(int numerator, int denominator){

}

void VST3Plugin::setTransportPlaying(bool play){

}

void VST3Plugin::setTransportRecording(bool record){

}

void VST3Plugin::setTransportAutomationWriting(bool writing){

}

void VST3Plugin::setTransportAutomationReading(bool reading){

}

void VST3Plugin::setTransportCycleActive(bool active){

}

void VST3Plugin::setTransportCycleStart(double beat){

}

void VST3Plugin::setTransportCycleEnd(double beat){

}

void VST3Plugin::setTransportPosition(double beat){

}

double VST3Plugin::getTransportPosition() const {
    return 0;
}

int VST3Plugin::getNumMidiInputChannels() const {
    return 0;
}

int VST3Plugin::getNumMidiOutputChannels() const {
    return 0;
}

bool VST3Plugin::hasMidiInput() const {
    return false;
}

bool VST3Plugin::hasMidiOutput() const {
    return false;
}

void VST3Plugin::sendMidiEvent(const VSTMidiEvent &event){

}

void VST3Plugin::sendSysexEvent(const VSTSysexEvent &event){

}

void VST3Plugin::setParameter(int index, float value){

}

bool VST3Plugin::setParameter(int index, const std::string &str){
    return false;
}

float VST3Plugin::getParameter(int index) const {
    return 0;
}

std::string VST3Plugin::getParameterName(int index) const {
    return std::string{};
}

std::string VST3Plugin::getParameterLabel(int index) const {
    return std::string{};
}

std::string VST3Plugin::getParameterDisplay(int index) const {
    return std::string{};
}

int VST3Plugin::getNumParameters() const {
    return 0;
}

void VST3Plugin::setProgram(int program){
}

void VST3Plugin::setProgramName(const std::string& name){
}

int VST3Plugin::getProgram() const {
    return 0;
}

std::string VST3Plugin::getProgramName() const {
    return std::string{};
}

std::string VST3Plugin::getProgramNameIndexed(int index) const {
    return std::string{};
}

int VST3Plugin::getNumPrograms() const {
    return 0;
}

bool VST3Plugin::hasChunkData() const {
    return false;
}

void VST3Plugin::setProgramChunkData(const void *data, size_t size){
}

void VST3Plugin::getProgramChunkData(void **data, size_t *size) const {
}

void VST3Plugin::setBankChunkData(const void *data, size_t size){
}

void VST3Plugin::getBankChunkData(void **data, size_t *size) const {
}

bool VST3Plugin::readProgramFile(const std::string& path){
    return false;
}

bool VST3Plugin::readProgramData(const char *data, size_t size){
    return false;
}

void VST3Plugin::writeProgramFile(const std::string& path){
}

void VST3Plugin::writeProgramData(std::string& buffer){

}

bool VST3Plugin::readBankFile(const std::string& path){
    return false;
}

bool VST3Plugin::readBankData(const char *data, size_t size){
    return false;
}

void VST3Plugin::writeBankFile(const std::string& path){

}

void VST3Plugin::writeBankData(std::string& buffer){

}

bool VST3Plugin::hasEditor() const {
    return false;
}

void VST3Plugin::openEditor(void * window){

}

void VST3Plugin::closeEditor(){

}

void VST3Plugin::getEditorRect(int &left, int &top, int &right, int &bottom) const {

}

// private
std::string VST3Plugin::getBaseName() const {
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

} // vst
