#include "VST3Plugin.h"
#include "Utility.h"

#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

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
        throw Error("couldn't load module!");
    }
    auto factoryProc = module_->getFnPtr<GetFactoryProc>("GetPluginFactory");
    if (!factoryProc){
        throw Error("couldn't find 'GetPluginFactory' function");
    }
    if (!module_->init()){
        throw Error("couldn't init module");
    }
    factory_ = IPtr<IPluginFactory>(factoryProc());
    if (!factory_){
        throw Error("couldn't get VST3 plug-in factory");
    }
    /// LOG_DEBUG("VST3Factory: loaded " << path);
    // map plugin names to indices
    auto numPlugins = factory_->countClasses();
    LOG_DEBUG("module contains " << numPlugins << " plugins");
    for (int i = 0; i < numPlugins; ++i){
        PClassInfo ci;
        if (factory_->getClassInfo(i, &ci) == kResultTrue){
            pluginMap_[ci.name] = i;
            LOG_DEBUG("\t" << ci.name);
        } else {
            LOG_ERROR("couldn't get class info!");
        }
    }
}

VST3Factory::~VST3Factory(){
    if (!module_->exit()){
        // don't throw!
        LOG_ERROR("couldn't exit module");
    }
    LOG_DEBUG("freed VST3 module " << path_);
}

void VST3Factory::addPlugin(PluginInfo::ptr desc){
    if (!pluginMap_.count(desc->name)){
        plugins_.push_back(desc);
        pluginMap_[desc->name] = plugins_.size() - 1;
    }
}

PluginInfo::const_ptr VST3Factory::getPlugin(int index) const {
    if (index >= 0 && index < (int)plugins_.size()){
        return plugins_[index];
    } else {
        return nullptr;
    }
}

int VST3Factory::numPlugins() const {
    return plugins_.size();
}

// for testing we don't want to load hundreds of shell plugins
#define SHELL_PLUGIN_LIMIT 1000
// probe subplugins asynchronously with futures or worker threads
#define PROBE_FUTURES 8 // number of futures to wait for
#define PROBE_THREADS 8 // number of worker threads (0: use futures instead of threads)

IFactory::ProbeFuture VST3Factory::probeAsync() {
    plugins_.clear();
    auto f = probePlugin(""); // don't need a name
    /// LOG_DEBUG("got probePlugin future");
    return [this, f=std::move(f)](ProbeCallback callback){
        /// LOG_DEBUG("about to call probePlugin future");
        auto result = f();
        /// LOG_DEBUG("called probePlugin future");
        if (result->shellPlugins_.empty()){
            plugins_ = { result };
            valid_ = result->valid();
            if (callback){
                callback(*result, 0, 1);
            }
        } else {
            // shell plugin!
            int numPlugins = result->shellPlugins_.size();
        #ifdef SHELL_PLUGIN_LIMIT
            numPlugins = std::min<int>(numPlugins, SHELL_PLUGIN_LIMIT);
        #endif
#if !PROBE_THREADS
            /// LOG_DEBUG("numPlugins: " << numPlugins);
            std::vector<std::tuple<int, std::string, PluginInfo::Future>> futures;
            int i = 0;
            while (i < numPlugins){
                futures.clear();
                // probe the next n plugins
                int n = std::min<int>(numPlugins - i, PROBE_FUTURES);
                for (int j = 0; j < n; ++j, ++i){
                    auto& shell = result->shellPlugins_[i];
                    try {
                        /// LOG_DEBUG("probing '" << shell.name << "'");
                        futures.emplace_back(i, shell.name, probePlugin(shell.name, shell.id));
                    } catch (const Error& e){
                        // should we rather propagate the error and break from the loop?
                        LOG_ERROR("couldn't probe '" << shell.name << "': " << e.what());
                    }
                }
                // collect results
                for (auto& tup : futures){
                    int index;
                    std::string name;
                    PluginInfo::Future future;
                    std::tie(index, name, future) = tup;
                    try {
                        auto plugin = future(); // wait for process
                        plugins_.push_back(plugin);
                        // factory is valid if contains at least 1 valid plugin
                        if (plugin->valid()){
                            valid_ = true;
                        }
                        if (callback){
                            callback(*plugin, index, numPlugins);
                        }
                    } catch (const Error& e){
                        // should we rather propagate the error and break from the loop?
                        LOG_ERROR("couldn't probe '" << name << "': " << e.what());
                    }
                }
            }
        }
#else
            /// LOG_DEBUG("numPlugins: " << numPlugins);
            auto& shellPlugins = result->shellPlugins_;
            auto next = shellPlugins.begin();
            auto end = next + numPlugins;
            int count = 0;
            std::deque<std::tuple<int, std::string, PluginInfo::ptr, Error>> results;

            std::mutex mutex;
            std::condition_variable cond;
            int numThreads = std::min<int>(numPlugins, PROBE_THREADS);
            std::vector<std::thread> threads;
            // thread function
            auto threadFun = [&](int i){
                std::unique_lock<std::mutex> lock(mutex);
                while (next != end){
                    auto shell = next++;
                    lock.unlock();
                    try {
                        /// LOG_DEBUG("probing '" << shell.name << "'");
                        auto plugin = probePlugin(shell->name, shell->id)();
                        lock.lock();
                        results.emplace_back(count++, shell->name, plugin, Error{});
                        /// LOG_DEBUG("thread " << i << ": probed " << shell->name);
                    } catch (const Error& e){
                        lock.lock();
                        results.emplace_back(count++, shell->name, nullptr, e);
                    }
                    cond.notify_one();
                }
                /// LOG_DEBUG("worker thread " << i << " finished");
            };
            // spawn worker threads
            for (int j = 0; j < numThreads; ++j){
                threads.push_back(std::thread(threadFun, j));
            }
            // collect results
            std::unique_lock<std::mutex> lock(mutex);
            while (true) {
                // process available data
                while (results.size() > 0){
                    int index;
                    std::string name;
                    PluginInfo::ptr plugin;
                    Error e;
                    std::tie(index, name, plugin, e) = results.front();
                    results.pop_front();
                    lock.unlock();

                    if (plugin){
                        plugins_.push_back(plugin);
                        // factory is valid if contains at least 1 valid plugin
                        if (plugin->valid()){
                            valid_ = true;
                        }
                        if (callback){
                            callback(*plugin, index, numPlugins);
                        }
                    } else {
                        // should we rather propagate the error and break from the loop?
                        LOG_ERROR("couldn't probe '" << name << "': " << e.what());
                    }
                    lock.lock();
                }
                if (count < numPlugins) {
                    /// LOG_DEBUG("wait...");
                    cond.wait(lock); // wait for more
                }
                else {
                    break; // done
                }
            }

            lock.unlock(); // !
            /// LOG_DEBUG("exit loop");
            // join worker threads
            for (auto& thread : threads){
                if (thread.joinable()){
                    thread.join();
                }
            }
            /// LOG_DEBUG("all worker threads joined");
#endif
        }
        for (int i = 0; i < (int)plugins_.size(); ++i){
            pluginMap_[plugins_[i]->name] = i;
        }
    };
}

std::unique_ptr<IPlugin> VST3Factory::create(const std::string& name, bool probe) const {
    auto it = pluginMap_.find(name);
    if (it == pluginMap_.end()){
        return nullptr;
    }
    auto which = it->second;
    if (!probe){
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
        return std::make_unique<VST3Plugin>(factory_, which, plugins_[which]);
    } catch (const Error& e){
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

VST3Plugin::VST3Plugin(IPtr<IPluginFactory> factory, int which, PluginInfo::const_ptr desc)
    : desc_(std::move(desc))
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
        throw Error("couldn't create VST3 component");
    }
    LOG_DEBUG("created VST3 component");
}

VST3Plugin::~VST3Plugin(){

}

std::string VST3Plugin::getPluginName() const {
    return name_;
}

std::string VST3Plugin::getPluginVendor() const {
    return vendor_;
}

std::string VST3Plugin::getPluginCategory() const {
    return category_;
}

std::string VST3Plugin::getPluginVersion() const {
    return version_;
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

intptr_t VST3Plugin::vendorSpecific(int index, intptr_t value, void *p, float opt){
    return 0;
}

void VST3Plugin::process(const float **inputs, float **outputs, int sampleFrames){

}

void VST3Plugin::processDouble(const double **inputs, double **outputs, int sampleFrames){

}

bool VST3Plugin::hasPrecision(ProcessPrecision precision) const {
    return false;
}

void VST3Plugin::setPrecision(ProcessPrecision precision){

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

void VST3Plugin::sendMidiEvent(const MidiEvent &event){

}

void VST3Plugin::sendSysexEvent(const SysexEvent &event){

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

void VST3Plugin::readProgramFile(const std::string& path){

}

void VST3Plugin::readProgramData(const char *data, size_t size){

}

void VST3Plugin::writeProgramFile(const std::string& path){

}

void VST3Plugin::writeProgramData(std::string& buffer){

}

void VST3Plugin::readBankFile(const std::string& path){

}

void VST3Plugin::readBankData(const char *data, size_t size){

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
#if 0
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
#endif

} // vst
