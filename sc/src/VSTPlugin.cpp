#include "VSTPlugin.h"

#ifdef SUPERNOVA
#include <nova-tt/spin_lock.hpp>
#endif

static InterfaceTable *ft;

namespace rt {
    InterfaceTable* interfaceTable;
}

void SCLog(const std::string& msg){
    Print(msg.c_str());
}

// SndBuffers
static void syncBuffer(World *world, int32 index) {
    auto src = world->mSndBufsNonRealTimeMirror + index;
    auto dest = world->mSndBufs + index;
    dest->samplerate = src->samplerate;
    dest->sampledur = src->sampledur;
    dest->data = src->data;
    dest->channels = src->channels;
    dest->samples = src->samples;
    dest->frames = src->frames;
    dest->mask = src->mask;
    dest->mask1 = src->mask1;
    dest->coord = src->coord;
    dest->sndfile = src->sndfile;
#ifdef SUPERNOVA
    dest->isLocal = src->isLocal;
#endif
    world->mSndBufUpdates[index].writes++;
}

static void allocReadBuffer(SndBuf* buf, const std::string& data) {
    auto n = data.size();
    BufAlloc(buf, 1, n, 1.0);
    for (int i = 0; i < n; ++i) {
        buf->data[i] = data[i];
    }
}

static void writeBuffer(SndBuf* buf, std::string& data) {
    auto n = buf->frames;
    data.resize(n);
    for (int i = 0; i < n; ++i) {
        data[i] = buf->data[i];
    }
}

template<typename T>
T* CmdData::create(World *world, int size) {
    auto data = RTAlloc(world, sizeof(T) + size);
    if (data) {
        new (data)T();
        return (T*)data;
    }
    else {
        LOG_ERROR("RTAlloc failed!");
        return nullptr;
    }
}

// Check if the Unit is still alive. Should only be called in RT stages!
bool CmdData::alive() const {
    auto b = owner->alive();
    if (!b) {
        LOG_WARNING("VSTPlugin: freed during background task");
    }
    return b;
}

// InfoCmdData
InfoCmdData* InfoCmdData::create(World* world, const char* path) {
    auto data = CmdData::create<InfoCmdData>(world);
    if (data) {
        snprintf(data->path, sizeof(data->path), "%s", path);
    }
    return data;
}

InfoCmdData* InfoCmdData::create(World* world, int bufnum) {
    auto data = CmdData::create<InfoCmdData>(world);
    if (data) {
        data->bufnum = bufnum;
        data->path[0] = '\0';
    }
    return data;
}

bool InfoCmdData::nrtFree(World* inWorld, void* cmdData) {
    auto data = (InfoCmdData*)cmdData;
    // this is potentially dangerous because NRTFree internally uses free()
    // while BufFreeCmd::Stage4() uses free_aligned().
    // On the other hand, the client is supposed to pass an *unused* bufnum,
    // so ideally we don't have to free any previous data.
    // The SndBuf is then freed by the client.
    if (data->freeData)
        NRTFree(data->freeData);
    return true;
}

// PluginCmdData
PluginCmdData* PluginCmdData::create(World *world, const char* path) {
    size_t size = path ? (strlen(path) + 1) : 0; // keep trailing '\0'!
    auto cmdData = CmdData::create<PluginCmdData>(world, size);
    if (cmdData) {
        if (path) {
            memcpy(cmdData->buf, path, size);
        }
        cmdData->size = size;
    }
    return cmdData;
}

// Encode a string as a list of floats.
// This is needed because the current plugin API only
// allows float arrays as arguments to Node replies.
// Format: size, ASCII chars...
int string2floatArray(const std::string& src, float *dest, int maxSize) {
    int len = std::min<int>(src.size(), maxSize-1);
    if (len >= 0) {
        *dest++ = len;
        for (int i = 0; i < len; ++i) {
            dest[i] = src[i];
        }
        return len + 1;
    }
    else {
        return 0;
    }
}

// search and probe
static bool gSearching = false;

static PluginManager gPluginManager;

#define SETTINGS_DIR ".VSTPlugin"
#define SETTINGS_FILE "plugins.ini"

static std::string getSettingsDir(){
#ifdef _WIN32
    return expandPath("%USERPROFILE%\\" SETTINGS_DIR);
#else
    return expandPath("~/" SETTINGS_DIR);
#endif
}

static void readIniFile(){
    try {
        gPluginManager.read(getSettingsDir() + "/" SETTINGS_FILE);
    } catch (const Error& e){
        LOG_ERROR("couldn't read settings file: " << e.what());
    }
}

static void writeIniFile(){
    try {
        auto dir = getSettingsDir();
        if (!pathExists(dir)){
            if (!createDirectory(dir)){
                throw Error("couldn't create directory");
            }
        }
        gPluginManager.write(dir + "/" SETTINGS_FILE);
    } catch (const Error& e){
        LOG_ERROR("couldn't write settings file: " << e.what());
    }
}

// VST2: plug-in name
// VST3: plug-in name + ".vst3"
static std::string makeKey(const PluginInfo& desc) {
    std::string key;
    auto ext = ".vst3";
    auto onset = std::max<size_t>(0, desc.path.size() - strlen(ext));
    if (desc.path.find(ext, onset) != std::string::npos) {
        key = desc.name + ext;
    }
    else {
        key = desc.name;
    }
    return key;
}

void serializePlugin(std::ostream& os, const PluginInfo& desc) {
    desc.serialize(os);
    os << "[keys]\n";
    os << "n=1\n";
    os << makeKey(desc) << "\n";
}

// load factory and probe plugins

static IFactory::ptr loadFactory(const std::string& path, bool verbose = false){
    IFactory::ptr factory;

    if (gPluginManager.findFactory(path)) {
        LOG_ERROR("ERROR: bug in 'loadFactory'");
        return nullptr;
    }
    if (gPluginManager.isException(path)) {
        if (verbose) {
            Print("'%s' is black-listed.\n", path.c_str());
        }
        return nullptr;
    }
    // load factory and probe plugins
    try {
        factory = IFactory::load(path);
    } catch (const Error& e){
        if (verbose) {
            Print("couldn't load '%s': %s\n", path.c_str(), e.what());
        }
        gPluginManager.addException(path);
        return nullptr;
    }

    return factory;
}

static bool addFactory(const std::string& path, IFactory::ptr factory){
    if (factory->numPlugins() == 1) {
        auto plugin = factory->getPlugin(0);
        // factories with a single plugin can also be aliased by their file path(s)
        gPluginManager.addPlugin(plugin->path, plugin);
        gPluginManager.addPlugin(path, plugin);
    }
    if (factory->valid()) {
        gPluginManager.addFactory(path, factory);
        for (int i = 0; i < factory->numPlugins(); ++i) {
            auto plugin = factory->getPlugin(i);
            if (plugin->valid()) {
                gPluginManager.addPlugin(makeKey(*plugin), plugin);
                // LOG_DEBUG("added plugin " << plugin->name);
            }
        }
        return true;
    }
    else {
        gPluginManager.addException(path);
        return false;
    }
}

static void postResult(ProbeResult pr){
    switch (pr) {
    case ProbeResult::success:
        Print("ok!\n");
        break;
    case ProbeResult::fail:
        Print("failed!\n");
        break;
    case ProbeResult::crash:
        Print("crashed!\n");
        break;
    default:
        Print("bug: probePlugin\n");
        break;
    }
}

static IFactory::ptr probePlugin(const std::string& path, bool verbose) {
    auto factory = loadFactory(path, verbose);
    if (!factory){
        return nullptr;
    }

    if (verbose) Print("probing %s... ", path.c_str());

    try {
        factory->probe([&](const PluginInfo & desc, int which, int numPlugins) {
            if (verbose) {
                if (numPlugins > 1) {
                    if (which == 0) {
                        Print("\n");
                    }
                    Print("\t[%d/%d] ", which + 1, numPlugins);
                    if (!desc.name.empty()) {
                        Print("'%s' ... ", desc.name.c_str());
                    }
                    else {
                        Print("plugin "); // e.g. "plugin crashed!"
                    }
                }
                postResult(desc.probeResult);
            }
        });
        if (addFactory(path, factory)){
            return factory; // success
        }
    }
    catch (const Error& e) {
        if (verbose) {
            Print("error!\n%s\n", e.what());
        }
    }
    return nullptr;
}

using FactoryFuture = std::function<IFactory::ptr()>;

static FactoryFuture nullFactoryFuture([](){ return nullptr; });

static FactoryFuture probePluginParallel(const std::string& path, bool verbose) {
    auto factory = loadFactory(path, verbose);
    if (!factory){
        return nullFactoryFuture;
    }
    try {
        // start probing process
        auto future = factory->probeAsync();
        // return future
        return [=]() -> IFactory::ptr {
            if (verbose) Print("probing %s... ", path.c_str());
            try {
                // wait for results
                future([&](const PluginInfo & desc, int which, int numPlugins) {
                    if (verbose) {
                        if (numPlugins > 1) {
                            if (which == 0) {
                                Print("\n");
                            }
                            Print("\t[%d/%d] ", which + 1, numPlugins);
                            if (!desc.name.empty()) {
                                Print("'%s' ... ", desc.name.c_str());
                            }
                            else {
                                Print("plugin "); // e.g. "plugin crashed!"
                            }
                        }
                        postResult(desc.probeResult);
                    }
                });
                // collect results
                if (addFactory(path, factory)){
                    return factory; // success
                }
            }
            catch (const Error& e) {
                if (verbose) {
                    Print("error!\n%s\n", e.what());
                }
            }
            return nullptr;
        };
    }
    catch (const Error& e) {
        if (verbose) {
            Print("error!\n%s\n", e.what());
        }
    }
    return nullFactoryFuture;
}


static bool isAbsolutePath(const std::string& path) {
    if (!path.empty() &&
        (path[0] == '/' || path[0] == '~' // Unix
#ifdef _WIN32
            || path[0] == '%' // environment variable
            || (path.size() >= 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) // drive
#endif
            )) return true;
    return false;
}

// resolves relative paths to an existing plugin in the canvas search paths or VST search paths.
// returns empty string on failure!
static std::string resolvePath(std::string path) {
    if (isAbsolutePath(path)) {
        return path; // success
    }
#ifdef _WIN32
    const char* ext = ".dll";
#elif defined(__APPLE__)
    const char* ext = ".vst";
#else
    const char* ext = ".so";
#endif
    if (path.find(".vst3") == std::string::npos && path.find(ext) == std::string::npos) {
        path += ext;
    }
    // otherwise try default VST paths
    for (auto& vstpath : getDefaultSearchPaths()) {
        auto result = vst::find(vstpath, path);
        if (!result.empty()) return result; // success
    }
    return std::string{}; // fail
}

// query a plugin by its key or file path and probe if necessary.
static const PluginInfo* queryPlugin(std::string path) {
#ifdef _WIN32
    for (auto& c : path) {
        if (c == '\\') c = '/';
    }
#endif
    // query plugin
    auto desc = gPluginManager.findPlugin(path);
    if (!desc) {
        // try as file path
        auto absPath = resolvePath(path);
        if (absPath.empty()){
            Print("'%s' is neither an existing plugin name "
                    "nor a valid file path.\n", path.c_str());
        } else if (!(desc = gPluginManager.findPlugin(absPath))){
            // finally probe plugin
            if (probePlugin(absPath, true)) {
                desc = gPluginManager.findPlugin(absPath);
                // findPlugin() fails if the module contains several plugins,
                // which means the path can't be used as a key.
                if (!desc){
                    Print("'%s' contains more than one plugin. "
                            "Please perform a search and open the desired "
                            "plugin by its name.\n", absPath.c_str());
                }
            }
        }
    }
    return desc.get();
}

#define PROBE_PROCESSES 8

std::vector<PluginInfo::const_ptr> searchPlugins(const std::string & path,
                                                 bool parallel, bool verbose) {
    Print("searching in '%s'...\n", path.c_str());
    std::vector<PluginInfo::const_ptr> results;

    auto addPlugin = [&](const PluginInfo::const_ptr& plugin, int which = 0, int n = 0){
        if (plugin->valid()) {
            if (verbose && n > 0) {
                Print("\t[%d/%d] %s\n", which + 1, n, plugin->name.c_str());
            }
            results.push_back(plugin);
        }
    };

    std::vector<FactoryFuture> futures;

    auto processFutures = [&](){
        for (auto& f : futures){
            auto factory = f();
            if (factory){
                int numPlugins = factory->numPlugins();
                for (int i = 0; i < numPlugins; ++i){
                    addPlugin(factory->getPlugin(i));
                }
            }
        }
        futures.clear();
    };

    vst::search(path, [&](const std::string & absPath, const std::string &) {
        std::string pluginPath = absPath;
#ifdef _WIN32
        for (auto& c : pluginPath) {
            if (c == '\\') c = '/';
        }
#endif
        // check if module has already been loaded
        auto factory = gPluginManager.findFactory(pluginPath);
        if (factory) {
            // just post names of valid plugins
            if (verbose) Print("%s\n", pluginPath.c_str());
            auto numPlugins = factory->numPlugins();
            if (numPlugins == 1) {
                addPlugin(factory->getPlugin(0));
            }
            else {
                if (verbose) LOG_VERBOSE(pluginPath);
                for (int i = 0; i < numPlugins; ++i) {
                    // add and post plugins
                    addPlugin(factory->getPlugin(i), i, numPlugins);
                }
            }
        }
        else {
            // probe (will post results and add plugins)
            if (parallel){
                futures.push_back(probePluginParallel(pluginPath, verbose));
                if (futures.size() >= PROBE_PROCESSES){
                    processFutures();
                }
            } else {
                if ((factory = probePlugin(pluginPath, verbose))) {
                    int numPlugins = factory->numPlugins();
                    for (int i = 0; i < numPlugins; ++i) {
                        addPlugin(factory->getPlugin(i));
                    }
                }
            }
        }
    });
    processFutures();

    int numResults = results.size();
    if (numResults == 1){
        Print("found 1 plugin\n");
    } else {
        Print("found %d plugins\n", numResults);
    }
    return results;
}

// -------------------- VSTPlugin ------------------------ //

VSTPlugin::VSTPlugin(){
    // UGen inputs: bypass, nin, inputs..., nparam, params...
    assert(numInputs() > 1);
    numInChannels_ = in0(1);
    int onset = inChannelOnset_ + numInChannels_;
    assert(numInputs() > onset);
    numParameterControls_ = in0(onset);
    parameterControlOnset_ = onset + 1;
    assert(numInputs() > (onset + numParameterControls_ * 2));
    // LOG_DEBUG("num in: " << numInChannels() << ", num out: " << numOutChannels() << ", num controls: " << numParameterControls_);
 
    // create delegate after member initialization!
    delegate_ = rt::make_shared<VSTPluginDelegate>(mWorld, *this);

    resizeBuffer();
    set_calc_function<VSTPlugin, &VSTPlugin::next>();

    // run queued unit commands
    if (queued_ == MagicQueued) {
        auto item = unitCmdQueue_;
        while (item) {
            sc_msg_iter args(item->size, item->data);
            // swallow the first 3 arguments
            args.geti(); // node ID
            args.geti(); // ugen index
            args.gets(); // unit command name
            (item->fn)((Unit*)this, &args);
            auto next = item->next;
            RTFree(mWorld, item);
            item = next;
        }
    }
}

VSTPlugin::~VSTPlugin(){
    if (buf_) RTFree(mWorld, buf_);
    if (inBufVec_) RTFree(mWorld, inBufVec_);
    if (outBufVec_) RTFree(mWorld, outBufVec_);
    if (paramState_) RTFree(mWorld, paramState_);
    if (paramMapping_) RTFree(mWorld, paramMapping_);
    clearMapping();
    // both variables are volatile, so the compiler is not allowed to optimize it away!
    initialized_ = 0;
    queued_ = 0;
    // tell the delegate that we've been destroyed!
    delegate_->setOwner(nullptr);
    delegate_ = nullptr; // release our reference
    LOG_DEBUG("destroyed VSTPlugin");
}

// HACK to check if the class has been fully constructed. See "runUnitCommand".
bool VSTPlugin::initialized() {
    return (initialized_ == MagicInitialized);
}

// Terrible hack to enable sending unit commands right after /s_new
// although the UGen constructor hasn't been called yet. See "runUnitCommand".
void VSTPlugin::queueUnitCmd(UnitCmdFunc fn, sc_msg_iter* args) {
    if (queued_ != MagicQueued) {
        unitCmdQueue_ = nullptr;
        queued_ = MagicQueued;
    }
    auto item = (UnitCmdQueueItem *)RTAlloc(mWorld, sizeof(UnitCmdQueueItem) + args->size);
    if (item) {
        item->next = nullptr;
        item->fn = fn;
        item->size = args->size;
        memcpy(item->data, args->data, args->size);
        // push to the back
        if (unitCmdQueue_) {
            auto tail = unitCmdQueue_;
            while (tail->next) tail = tail->next;
            tail->next = item;
        }
        else {
            unitCmdQueue_ = item;
        }
    }
}

void VSTPlugin::resizeBuffer(){
    bool fail = false;
    int blockSize = bufferSize();
    int nin = numInChannels();
    int nout = numOutChannels();
    int bufin = 0;
    int bufout = 0;
    auto plugin = delegate_->plugin();
    if (plugin){
        nin = std::max<int>(nin, plugin->getNumInputs());
        nout = std::max<int>(nout, plugin->getNumOutputs());
        // buffer extra inputs/outputs for safety
        bufin = std::max<int>(0, plugin->getNumInputs() - numInChannels());
        bufout = std::max<int>(0, plugin->getNumOutputs() - numOutChannels());
        LOG_DEBUG("bufin: " << bufin << ", bufout: " << bufout);
    }
    // safety buffer
    {
        int bufSize = (bufin + bufout) * blockSize * sizeof(float);
        if (bufSize > 0) {
            auto result = (float*)RTRealloc(mWorld, buf_, bufSize);
            if (result) {
                buf_ = result;
                memset(buf_, 0, bufSize);
            }
            else goto fail;
        }
        else {
            RTFree(mWorld, buf_);
            buf_ = nullptr;
        }
    }
    // input buffer array
    {
        if (nin > 0) {
            auto result = (const float**)RTRealloc(mWorld, inBufVec_, nin * sizeof(float*));
            if (result) {
                inBufVec_ = result;
                for (int i = 0; i < numInChannels(); ++i) {
                    inBufVec_[i] = in(i + inChannelOnset_);
                }
                // for safety:
                for (int i = 0; i < bufin; ++i) {
                    inBufVec_[numInChannels() + i] = &buf_[i * blockSize];
                }
            }
            else goto fail;
        }
        else {
            RTFree(mWorld, inBufVec_);
            inBufVec_ = nullptr;
        }
    }
    // output buffer array
    {
        if (nout > 0) {
            auto result = (float**)RTRealloc(mWorld, outBufVec_, nout * sizeof(float*));
            if (result) {
                outBufVec_ = result;
                for (int i = 0; i < numOutChannels(); ++i) {
                    outBufVec_[i] = out(i);
                }
                // for safety:
                for (int i = 0; i < bufout; ++i) {
                    outBufVec_[numOutChannels() + i] = &buf_[(i + bufin) * blockSize];
                }
            }
            else goto fail;
        }
        else {
            RTFree(mWorld, outBufVec_);
            outBufVec_ = nullptr;
        }
    }
    LOG_DEBUG("resized buffer");
    return;
fail:
    LOG_ERROR("RTRealloc failed!");
    RTFree(mWorld, buf_);
    RTFree(mWorld, inBufVec_);
    RTFree(mWorld, outBufVec_);
    buf_ = nullptr; inBufVec_ = nullptr; outBufVec_ = nullptr;
}

void VSTPlugin::clearMapping() {
    auto m = paramMappingList_;
    while (m != nullptr) {
        auto next = m->next;
        RTFree(mWorld, m);
        m = next;
    }
    paramMapping_ = nullptr;
}

float VSTPlugin::readControlBus(int32 num) {
    if (num >= 0 && num < mWorld->mNumControlBusChannels) {
#define unit this
        ACQUIRE_BUS_CONTROL(num);
        float value = mWorld->mControlBus[num];
        RELEASE_BUS_CONTROL(num);
        return value;
#undef unit
    }
    else {
        return 0.f;
    }
}

// update data (after loading a new plugin)
void VSTPlugin::update() {
    resizeBuffer();
    clearMapping();
    int n = delegate().plugin()->getNumParameters();
    // parameter states
    {
        auto result = (float*)RTRealloc(mWorld, paramState_, n * sizeof(float));
        if (result) {
            for (int i = 0; i < n; ++i) {
                result[i] = std::numeric_limits<float>::quiet_NaN();
            }
            paramState_ = result;
        }
        else {
            RTFree(mWorld, paramState_);
            paramState_ = nullptr;
            LOG_ERROR("RTRealloc failed!");
        }
    }
    // parameter mapping
    {
        auto result = (Mapping**)RTRealloc(mWorld, paramMapping_, n * sizeof(Mapping *));
        if (result) {
            for (int i = 0; i < n; ++i) {
                result[i] = nullptr;
            }
            paramMapping_ = result;
        }
        else {
            RTFree(mWorld, paramMapping_);
            paramMapping_ = nullptr;
            LOG_ERROR("RTRealloc failed!");
        }
    }
}

void VSTPlugin::map(int32 index, int32 bus) {
    if (paramMapping_[index] == nullptr) {
        auto mapping = (Mapping*)RTAlloc(mWorld, sizeof(Mapping));
        if (mapping) {
            // add to head of linked list
            mapping->index = index;
            mapping->bus = bus;
            mapping->prev = nullptr;
            mapping->next = paramMappingList_;
            if (paramMappingList_) {
                paramMappingList_->prev = mapping;
            }
            paramMappingList_ = mapping;
            paramMapping_[index] = mapping;
        }
        else {
            LOG_ERROR("RTAlloc failed!");
        }
    }
}

void VSTPlugin::unmap(int32 index) {
    auto mapping = paramMapping_[index];
    if (mapping) {
        // remove from linked list
        if (mapping->prev) {
            mapping->prev->next = mapping->next;
        }
        else { // head
            paramMappingList_ = mapping->next;
        }
        if (mapping->next) {
            mapping->next->prev = mapping->prev;
        }
        RTFree(mWorld, mapping);
        paramMapping_[index] = nullptr;
    }
}

// perform routine
void VSTPlugin::next(int inNumSamples) {
    if (!(inBufVec_ || outBufVec_)) {
        // only if RT memory methods failed in resizeBuffer()
        (*ft->fClearUnitOutputs)(this, inNumSamples);
        return;
    }
    int nin = numInChannels();
    int nout = numOutChannels();
    bool bypass = in0(0);
    int offset = 0;
    auto plugin = delegate_->plugin();
    // disable reset for realtime-safety reasons
#if 0
    // only reset plugin when bypass changed from true to false
    if (plugin && !bypass && (bypass != bypass_)) {
        reset();
    }
    bypass_ = bypass;
#endif

    if (plugin && !bypass && plugin->hasPrecision(ProcessPrecision::Single)) {
        if (paramState_) {
            int nparam = plugin->getNumParameters();
            // update parameters from mapped control busses
            for (auto m = paramMappingList_; m != nullptr; m = m->next) {
                int index = m->index;
                int bus = m->bus;
                float value = readControlBus(bus);
                assert(index >= 0 && index < nparam);
                if (value != paramState_[index]) {
                    plugin->setParameter(index, value);
                    paramState_[index] = value;
                }
            }
            // update parameters from UGen inputs
            for (int i = 0; i < numParameterControls_; ++i) {
                int k = 2 * i + parameterControlOnset_;
                int index = in0(k);
                float value = in0(k + 1);
                // only if index is not out of range and the parameter is not mapped to a bus
                if (index >= 0 && index < nparam && paramMapping_[index] == nullptr
                    && paramState_[index] != value)
                {
                    plugin->setParameter(index, value);
                    paramState_[index] = value;
                }
            }
        }
        // process
        plugin->process((const float**)inBufVec_, outBufVec_, inNumSamples);
        offset = plugin->getNumOutputs();

#if VSTTHREADS
        // send parameter automation notification posted from the GUI thread.
        // we assume this is only possible if we have a VST editor window.
        // try_lock() won't block the audio thread and we don't mind if notifications
        // will be delayed if try_lock() fails (which happens rarely in practice).
        if (plugin->getWindow() && mutex_.try_lock()) {
            std::vector<std::pair<int, float>> queue;
            queue.swap(paramQueue_);
            mutex_.unlock();
            for (auto& p : queue) {
                delegate_->sendParameterAutomated(p.first, p.second);
            }
        }
#endif
    }
    else {
        // bypass (copy input to output)
        int n = std::min(nin, nout);
        for (int i = 0; i < n; ++i) {
            Copy(inNumSamples, outBufVec_[i], (float*)inBufVec_[i]);
        }
        offset = n;
    }
    // zero remaining outlets
    for (int i = offset; i < nout; ++i) {
        Fill(inNumSamples, outBufVec_[i], 0.f);
    }
}


//------------------- VSTPluginDelegate ------------------------------//

VSTPluginDelegate::VSTPluginDelegate(VSTPlugin& owner) {
    setOwner(&owner);
    rtThreadID_ = std::this_thread::get_id();
    // LOG_DEBUG("RT thread ID: " << rtThreadID_);
}

VSTPluginDelegate::~VSTPluginDelegate() {
    if (pluginUseCount_ > 0) {
        LOG_ERROR("BUG: pluginUseCount_ not 0 in destructor!");
        return;
    }
    close();
    LOG_DEBUG("VSTPluginDelegate destroyed");
}

bool VSTPluginDelegate::alive() const {
    return owner_ != nullptr;
}

void VSTPluginDelegate::setOwner(VSTPlugin *owner) {
    if (owner) {
        // cache some members
        world_ = owner->mWorld;
        // these are needed in cmdOpen (so we don't have to touch VSTPlugin
        // which might get destroyed concurrently in the RT thread).
        // NOTE: this might get called in the VSTPlugin's constructor,
        // so we have to make sure that those methods return a valid result (they do).
        sampleRate_ = owner->sampleRate();
        bufferSize_ = owner->bufferSize();
        numInChannels_ = owner->numInChannels();
        numOutChannels_ = owner->numOutChannels();
    }
    owner_ = owner;
}

// NOTE: in case we don't have a GUI thread we *could* get rid of nrtThreadID
// and just assume that std::this_thread::get_id() != rtThreadID_ means
// we're on the NRT thread - but I don't know if can be 100% sure about this,
// so let's play it safe.
void VSTPluginDelegate::parameterAutomated(int index, float value) {
    // RT thread
    if (std::this_thread::get_id() == rtThreadID_) {
        LOG_DEBUG("parameterAutomated (RT): " << index << ", " << value);
#if 0
        // linked parameters automated by control busses or Ugens
        sendParameterAutomated(index, value);
#endif
    }
    // NRT thread
    else if (std::this_thread::get_id() == nrtThreadID_) {
        LOG_DEBUG("parameterAutomated (NRT): " << index << ", " << value);
        FifoMsg msg;
        auto data = new ParamCmdData;
        data->owner = shared_from_this();
        data->index = index;
        data->value = value;

        msg.Set(world(), // world
            [](FifoMsg * msg) { // perform
            auto data = (ParamCmdData*)msg->mData;
            if (data->alive()) {
                // send parameter
                data->owner->sendParameterAutomated(data->index, data->value);
            }
            // release in RT thread
            data->owner = nullptr;
        }, [](FifoMsg * msg) { // free
            delete (ParamCmdData*)msg->mData;
        }, data); // data

        SendMsgToRT(world(), msg);
    }
#if VSTTHREADS
    // GUI thread (neither RT nor NRT thread) - push to queue
    else {
        LOG_DEBUG("parameterAutomated (GUI): " << index << ", " << value);
        std::lock_guard<std::mutex> guard(owner_->mutex_);
        owner_->paramQueue_.emplace_back(index, value);
    }
#endif
}

void VSTPluginDelegate::midiEvent(const MidiEvent& midi) {
#if VSTTHREADS
    // check if we're on the realtime thread, otherwise ignore it
    if (std::this_thread::get_id() == rtThreadID_) {
#else
    {
#endif
        float buf[3];
        // we don't want negative values here
        buf[0] = (unsigned char)midi.data[0];
        buf[1] = (unsigned char)midi.data[1];
        buf[2] = (unsigned char)midi.data[2];
        sendMsg("/vst_midi", 3, buf);
    }
}

void VSTPluginDelegate::sysexEvent(const SysexEvent & sysex) {
#if VSTTHREADS
    // check if we're on the realtime thread, otherwise ignore it
    if (std::this_thread::get_id() == rtThreadID_) {
#else
    {
#endif
        auto& data = sysex.data;
        int size = data.size();
        if ((size * sizeof(float)) > MAX_OSC_PACKET_SIZE) {
            LOG_WARNING("sysex message (" << size << " bytes) too large for UDP packet - dropped!");
            return;
        }
        float* buf = (float*)RTAlloc(world(), size * sizeof(float));
        if (buf) {
            for (int i = 0; i < size; ++i) {
                // no need to cast to unsigned because SC's Int8Array is signed anyway
                buf[i] = data[i];
            }
            sendMsg("/vst_sysex", size, buf);
            RTFree(world(), buf);
        }
        else {
            LOG_ERROR("RTAlloc failed!");
        }
    }
}

bool VSTPluginDelegate::check() {
    if (plugin_) {
        return true;
    }
    else {
        LOG_WARNING("VSTPlugin: no plugin loaded!");
        return false;
    }
}

// try to close the plugin in the NRT thread with an asynchronous command
void VSTPluginDelegate::close() {
    if (plugin_) {
        LOG_DEBUG("about to close");
        if (pluginUseCount_ > 0) {
            LOG_WARNING("VSTPlugin: can't close plugin while commands are still running");
            return;
        }
        auto cmdData = PluginCmdData::create(world());
        if (!cmdData) {
            return;
        }
        cmdData->plugin = std::move(plugin_);
        cmdData->value = editor_;
        // don't set owner!
        doCmd<false>(cmdData, [](World *world, void* inData) {
            auto data = (PluginCmdData*)inData;
            try {
                if (data->value) {
                    UIThread::destroy(std::move(data->plugin));
                }
                else {
                    data->plugin = nullptr; // destruct
                }
            }
            catch (const Error & e) {
                LOG_ERROR("ERROR: couldn't close plugin: " << e.what());
            }
            return false; // done
        });
        plugin_ = nullptr;
    }
}

bool cmdOpen(World *world, void* cmdData) {
    LOG_DEBUG("cmdOpen");
    // initialize GUI backend (if needed)
    auto data = (PluginCmdData *)cmdData;
    data->threadID = std::this_thread::get_id();
    // create plugin in main thread
    auto info = queryPlugin(data->buf);
    if (info && info->valid()) {
        try {
            IPlugin::ptr plugin;
            bool editor = data->value;
            if (editor) {
                plugin = UIThread::create(*info);
            }
            else {
                plugin = info->create();
            }
            auto owner = data->owner;
            plugin->suspend();
            // we only access immutable members of owner
            plugin->setSampleRate(owner->sampleRate());
            plugin->setBlockSize(owner->bufferSize());
            // LOG_DEBUG("sr: " << owner->sampleRate() << ", bs: " << owner->bufferSize());
            if (plugin->hasPrecision(ProcessPrecision::Single)) {
                plugin->setPrecision(ProcessPrecision::Single);
            }
            else {
                LOG_WARNING("VSTPlugin: plugin '" << plugin->getPluginName() << "' doesn't support single precision processing - bypassing!");
            }
            int nin = std::min<int>(plugin->getNumInputs(), owner->numInChannels());
            int nout = std::min<int>(plugin->getNumOutputs(), owner->numOutChannels());
            plugin->setNumSpeakers(nin, nout);
            // LOG_DEBUG("nin: " << nin << ", nout: " << nout);
            plugin->resume();
            data->plugin = std::move(plugin);
        }
        catch (const Error & e) {
            LOG_ERROR(e.what());
        }
    }
    return true;
}

bool cmdOpenDone(World* world, void* cmdData) {
    auto data = (PluginCmdData*)cmdData;
    data->owner->doneOpen(*data); // alive() checked in doneOpen!
    return false; // done
}

// try to open the plugin in the NRT thread with an asynchronous command
void VSTPluginDelegate::open(const char *path, bool gui) {
    LOG_DEBUG("open");
    if (isLoading_) {
        LOG_WARNING("already loading!");
        return;
    }
    close();
    if (plugin_) {
        LOG_ERROR("ERROR: couldn't close current plugin!");
        return;
    }

    auto cmdData = PluginCmdData::create(world(), path);
    if (cmdData) {
        cmdData->value = gui;
        doCmd(cmdData, cmdOpen, cmdOpenDone);
        editor_ = gui;
        isLoading_ = true;
    }
}

// "/open" command succeeded/failed - called in the RT thread
void VSTPluginDelegate::doneOpen(PluginCmdData& cmd){
    LOG_DEBUG("doneOpen");
    isLoading_ = false;
    // move the plugin even if alive() returns false (so it will be properly released in close())
    plugin_ = std::move(cmd.plugin);
    nrtThreadID_ = cmd.threadID;
    if (!alive()) {
        LOG_WARNING("VSTPlugin: freed during background task");
    }
    // LOG_DEBUG("NRT thread ID: " << nrtThreadID_);
    if (plugin_){
        LOG_DEBUG("opened " << cmd.buf);
        // receive events from plugin
        plugin_->setListener(shared_from_this());
        // update data
        owner_->update();
        // success, window
        float data[] = { 1.f, static_cast<float>(plugin_->getWindow() != nullptr) };
        sendMsg("/vst_open", 2, data);
    } else {
        LOG_WARNING("VSTPlugin: couldn't open " << cmd.buf);
        sendMsg("/vst_open", 0);
    }
}

void VSTPluginDelegate::showEditor(bool show) {
    if (plugin_ && plugin_->getWindow()) {
        auto cmdData = PluginCmdData::create(world());
        if (cmdData) {
            cmdData->value = show;
            doCmd(cmdData, [](World * inWorld, void* inData) {
                auto data = (PluginCmdData*)inData;
                auto window = data->owner->plugin()->getWindow();
                if (data->value) {
                    window->bringToTop();
                }
                else {
                    window->hide();
                }
                return false; // done
            });
        }
    }
}

// some plugins crash when being reset reset
// in the NRT thread. we let the user choose
// and add a big fat warning in the help file.
bool cmdReset(World *world, void *cmdData) {
    auto data = (PluginCmdData *)cmdData;
    data->owner->plugin()->suspend();
    data->owner->plugin()->resume();
    return false; // done
}

void VSTPluginDelegate::reset(bool async) {
    if (check()) {
        if (async) {
            // reset in the NRT thread (unsafe)
            doCmd(PluginCmdData::create(world()), cmdReset);
        }
        else {
            // reset in the RT thread (safe)
            plugin_->suspend();
            plugin_->resume();
        }
    }
}

bool cmdSetParam(World *world, void *cmdData) {
    auto data = (ParamCmdData *)cmdData;
    auto index = data->index;
    if (data->display[0]) {
        data->owner->plugin()->setParameter(index, data->display);
    }
    else {
        data->owner->plugin()->setParameter(index, data->value);
    }
    return true;
}

bool cmdSetParamDone(World *world, void *cmdData) {
    auto data = (ParamCmdData *)cmdData;
    if (!data->alive()) return false;
    data->owner->setParamDone(data->index);
    return false; // done
}

void VSTPluginDelegate::setParam(int32 index, float value) {
    if (check()) {
        if (index >= 0 && index < plugin_->getNumParameters()) {
            auto data = CmdData::create<ParamCmdData>(world());
            if (data) {
                data->index = index;
                data->value = value;
                data->display[0] = 0;
                doCmd(data, cmdSetParam, cmdSetParamDone);
            }
            else {
                LOG_ERROR("RTAlloc failed!");
            }
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::setParam(int32 index, const char *display) {
    if (check()) {
        if (index >= 0 && index < plugin_->getNumParameters()) {
            auto len = strlen(display) + 1;
            auto data = CmdData::create<ParamCmdData>(world(), len);
            if (data) {
                data->index = index;
                data->value = 0;
                memcpy(data->display, display, len);
                doCmd(data, cmdSetParam, cmdSetParamDone);
            }
            else {
                LOG_ERROR("RTAlloc failed!");
            }
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::setParamDone(int32 index) {
    owner_->paramState_[index] = plugin_->getParameter(index);
    owner_->unmap(index);
    sendParameter(index);
}

void VSTPluginDelegate::queryParams(int32 index, int32 count) {
    if (check()) {
        int32 nparam = plugin_->getNumParameters();
        if (index >= 0 && index < nparam) {
            count = std::min<int32>(count, nparam - index);
            for (int i = 0; i < count; ++i) {
                sendParameter(index + i);
            }
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::getParam(int32 index) {
    if (check()) {
        if (index >= 0 && index < plugin_->getNumParameters()) {
            float value = plugin_->getParameter(index);
            sendMsg("/vst_set", value);
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::getParams(int32 index, int32 count) {
    if (check()) {
        int32 nparam = plugin_->getNumParameters();
        if (index >= 0 && index < nparam) {
            count = std::min<int32>(count, nparam - index);
            const int bufsize = count + 1;
            float *buf = (float *)RTAlloc(world(), sizeof(float) * bufsize);
            if (buf) {
                buf[0] = count;
                for (int i = 0; i < count; ++i) {
                    float value = plugin_->getParameter(i + index);
                    buf[i + 1] = value;
                }
                sendMsg("/vst_setn", bufsize, buf);
                RTFree(world(), buf);
            }
            else {
                LOG_ERROR("RTAlloc failed!");
            }
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::mapParam(int32 index, int32 bus) {
    if (check()) {
        if (index >= 0 && index < plugin_->getNumParameters()) {
            owner_->map(index, bus);
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::unmapParam(int32 index) {
    if (check()) {
        if (index >= 0 && index < plugin_->getNumParameters()) {
            owner_->unmap(index);
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

// program/bank
bool cmdSetProgram(World *world, void *cmdData) {
    auto data = (PluginCmdData *)cmdData;
    data->owner->plugin()->setProgram(data->value);
    return true;
}

bool cmdSetProgramDone(World *world, void *cmdData) {
    auto data = (PluginCmdData *)cmdData;
    if (!data->alive()) return false;
    data->owner->sendMsg("/vst_program_index", data->owner->plugin()->getProgram());
    return false; // done
}

void VSTPluginDelegate::setProgram(int32 index) {
    if (check()) {
        if (index >= 0 && index < plugin_->getNumPrograms()) {
            auto data = PluginCmdData::create(world());
            if (data) {
                data->value = index;
                doCmd(data, cmdSetProgram, cmdSetProgramDone);
            }
            else {
                LOG_ERROR("RTAlloc failed!");
            }
        }
        else {
            LOG_WARNING("VSTPlugin: program number " << index << " out of range!");
        }
    }
}
void VSTPluginDelegate::setProgramName(const char *name) {
    if (check()) {
        plugin_->setProgramName(name);
        sendCurrentProgramName();
    }
}

void VSTPluginDelegate::queryPrograms(int32 index, int32 count) {
    if (check()) {
        int32 nprogram = plugin_->getNumPrograms();
        if (index >= 0 && index < nprogram) {
            count = std::min<int32>(count, nprogram - index);
#if 1
            for (int i = 0; i < count; ++i) {
                sendProgramName(index + i);
            }
#else
            auto old = plugin_->getProgram();
            bool changed;
            for (int i = 0; i < count; ++i) {
                changed = sendProgramName(index + i);
            }
            if (changed) {
                plugin_->setProgram(old);
            }
#endif
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

template<bool bank>
bool cmdReadPreset(World* world, void* cmdData) {
    auto data = (InfoCmdData*)cmdData;
    auto plugin = data->owner->plugin();
    bool result = true;
    try {
        if (data->bufnum < 0) {
            // from file
            if (bank)
                plugin->readBankFile(data->path);
            else
                plugin->readProgramFile(data->path);
        }
        else {
            // from buffer
            std::string presetData;
            auto buf = World_GetNRTBuf(world, data->bufnum);
            writeBuffer(buf, presetData);
            if (bank)
                plugin->readBankData(presetData);
            else
                plugin->readProgramData(presetData);
        }
    }
    catch (const Error& e) {
        Print("ERROR: couldn't read %s: %s\n", (bank ? "bank" : "program"), e.what());
        result = false;
    }
    data->flags = result;
    return true;
}

template<bool bank>
bool cmdReadPresetDone(World *world, void *cmdData){
    auto data = (InfoCmdData *)cmdData;
    if (!data->alive()) return false;
    auto owner = data->owner;
    if (bank) {
        owner->sendMsg("/vst_bank_read", data->flags);
        // a bank change also sets the current program number!
        owner->sendMsg("/vst_program_index", owner->plugin()->getProgram());
    }
    else {
        owner->sendMsg("/vst_program_read", data->flags);
    }
    // the program name has most likely changed
    owner->sendCurrentProgramName();
    return false; // done
}

template<bool bank>
void VSTPluginDelegate::readPreset(const char *path){
    if (check()){
        doCmd(InfoCmdData::create(world(), path),
            cmdReadPreset<bank>, cmdReadPresetDone<bank>);
    }
}

template<bool bank>
void VSTPluginDelegate::readPreset(int32 buf) {
    if (check()) {
        doCmd(InfoCmdData::create(world(), buf),
            cmdReadPreset<bank>, cmdReadPresetDone<bank>);
    }
}

template<bool bank>
bool cmdWritePreset(World *world, void *cmdData){
    auto data = (InfoCmdData *)cmdData;
    auto plugin = data->owner->plugin();
    bool result = true;
    try {
        if (data->bufnum < 0) {
            if (bank)
                plugin->writeBankFile(data->path);
            else
                plugin->writeProgramFile(data->path);
        }
        else {
            // to buffer
            std::string presetData;
            if (bank)
                plugin->writeBankData(presetData);
            else
                plugin->writeProgramData(presetData);
            auto buf = World_GetNRTBuf(world, data->bufnum);
            data->freeData = buf->data; // to be freed in stage 4
            allocReadBuffer(buf, presetData);
        }
    }
    catch (const Error & e) {
        Print("ERROR: couldn't write %s: %s\n", (bank ? "bank" : "program"), e.what());
        result = false;
    }
    data->flags = result;
    return true;
}

template<bool bank>
bool cmdWritePresetDone(World *world, void *cmdData){
    auto data = (InfoCmdData *)cmdData;
    if (!data->alive()) return true; // will just free data
    syncBuffer(world, data->bufnum);
    data->owner->sendMsg(bank ? "/vst_bank_write" : "/vst_program_write", data->flags);
    return true; // continue
}

template<bool bank>
void VSTPluginDelegate::writePreset(const char *path) {
    if (check()) {
        doCmd(InfoCmdData::create(world(), path),
            cmdWritePreset<bank>, cmdWritePresetDone<bank>, InfoCmdData::nrtFree);
    }
}

template<bool bank>
void VSTPluginDelegate::writePreset(int32 buf) {
    if (check()) {
        doCmd(InfoCmdData::create(world(), buf),
            cmdWritePreset<bank>, cmdWritePresetDone<bank>, InfoCmdData::nrtFree);
    }
}

// midi
void VSTPluginDelegate::sendMidiMsg(int32 status, int32 data1, int32 data2) {
    if (check()) {
        plugin_->sendMidiEvent(MidiEvent(status, data1, data2));
    }
}
void VSTPluginDelegate::sendSysexMsg(const char *data, int32 n) {
    if (check()) {
        plugin_->sendSysexEvent(SysexEvent(data, n));
    }
}
// transport
void VSTPluginDelegate::setTempo(float bpm) {
    if (check()) {
        plugin_->setTempoBPM(bpm);
    }
}
void VSTPluginDelegate::setTimeSig(int32 num, int32 denom) {
    if (check()) {
        plugin_->setTimeSignature(num, denom);
    }
}
void VSTPluginDelegate::setTransportPlaying(bool play) {
    if (check()) {
        plugin_->setTransportPlaying(play);
    }
}
void VSTPluginDelegate::setTransportPos(float pos) {
    if (check()) {
        plugin_->setTransportPosition(pos);
    }
}
void VSTPluginDelegate::getTransportPos() {
    if (check()) {
        float f = plugin_->getTransportPosition();
        sendMsg("/vst_transport", f);
    }
}

// advanced

void VSTPluginDelegate::canDo(const char *what) {
    if (check()) {
        auto result = plugin_->canDo(what);
        sendMsg("/vst_can_do", (float)result);
    }
}

bool cmdVendorSpecific(World *world, void *cmdData) {
    auto data = (VendorCmdData *)cmdData;
    auto result = data->owner->plugin()->vendorSpecific(data->index, data->value, data->data, data->opt);
    data->index = result; // save result
    return true;
}

bool cmdVendorSpecificDone(World *world, void *cmdData) {
    auto data = (VendorCmdData *)cmdData;
    if (!data->alive()) return false;
    data->owner->sendMsg("/vst_vendor_method", (float)(data->index));
    return false; // done
}

void VSTPluginDelegate::vendorSpecific(int32 index, int32 value, size_t size, const char *data, float opt, bool async) {
    if (check()) {
        if (async) {
            auto cmdData = CmdData::create<VendorCmdData>(world(), size);
            if (cmdData) {
                cmdData->index = index;
                cmdData->value = value;
                cmdData->opt = opt;
                cmdData->size = size;
                memcpy(cmdData->data, data, size);
                doCmd(cmdData, cmdVendorSpecific, cmdVendorSpecificDone);
            }
            else {
                LOG_ERROR("RTAlloc failed!");
            }
        }
        else {
            auto result = plugin_->vendorSpecific(index, value, (void *)data, opt);
            sendMsg("/vst_vendor_method", (float)result);
        }
    }
}

// unchecked
bool VSTPluginDelegate::sendProgramName(int32 num) {
    const int maxSize = 64;
    float buf[maxSize];
    bool changed = false;
    auto name = plugin_->getProgramNameIndexed(num);
#if 0
    // some old plugins don't support indexed program name lookup
    if (name.empty()) {
        plugin_->setProgram(num);
        name = plugin_->getProgramName();
        changed = true;
    }
#endif
    // msg format: index, len, characters...
    buf[0] = num;
    int size = string2floatArray(name, buf + 1, maxSize - 1);
    sendMsg("/vst_program", size + 1, buf);
    return changed;
}

void VSTPluginDelegate::sendCurrentProgramName() {
    const int maxSize = 64;
    float buf[maxSize];
    // msg format: index, len, characters...
    buf[0] = plugin_->getProgram();
    int size = string2floatArray(plugin_->getProgramName(), buf + 1, maxSize - 1);
    sendMsg("/vst_program", size + 1, buf);
}

// unchecked
void VSTPluginDelegate::sendParameter(int32 index) {
    const int maxSize = 64;
    float buf[maxSize];
    // msg format: index, value, display length, display chars...
    buf[0] = index;
    buf[1] = plugin_->getParameter(index);
    int size = string2floatArray(plugin_->getParameterDisplay(index), buf + 2, maxSize - 2);
    sendMsg("/vst_param", size + 2, buf);
}

// unchecked
void VSTPluginDelegate::sendParameterAutomated(int32 index, float value) {
    sendParameter(index);
    float buf[2] = { (float)index, value };
    sendMsg("/vst_auto", 2, buf);
}

void VSTPluginDelegate::sendMsg(const char *cmd, float f) {
    // LOG_DEBUG("sending msg: " << cmd);
    SendNodeReply(&owner_->mParent->mNode, owner_->mParentIndex, cmd, 1, &f);
}

void VSTPluginDelegate::sendMsg(const char *cmd, int n, const float *data) {
    // LOG_DEBUG("sending msg: " << cmd);
    if (owner_) {
        SendNodeReply(&owner_->mParent->mNode, owner_->mParentIndex, cmd, n, data);
    }
    else {
        LOG_ERROR("BUG: VSTPluginDelegate::sendMsg");
    }
}

void cmdRTfree(World *world, void * cmdData) {
    if (cmdData) {
        RTFree(world, cmdData);
        // LOG_DEBUG("cmdRTfree!");
    }
}

// 'clean' version for non-POD data
template<bool owner, typename T>
void cmdRTfree(World *world, void * cmdData) {
    if (cmdData) {
        auto data = (T*)cmdData;
        if (owner) {
            data->owner->unref();
        }
        data->~T(); // destruct members (e.g. release rt::shared_pointer in RT thread)
        RTFree(world, cmdData);
        LOG_DEBUG("cmdRTfree!");
    }
}

// if 'owner' parameter is false, we don't store a pointer to this and we don't touch the ref count.
// this is mainly for close(), where we don't touch any shared state because we might get called
// in the destructor!
template<bool owner, typename T>
void VSTPluginDelegate::doCmd(T *cmdData, AsyncStageFn stage2,
    AsyncStageFn stage3, AsyncStageFn stage4) {
    // so we don't have to always check the return value of makeCmdData
    if (cmdData) {
        if (owner) {
            ref();
            cmdData->owner = shared_from_this();
        }
        DoAsynchronousCommand(world(),
            0, 0, cmdData, stage2, stage3, stage4, cmdRTfree<owner, T>, 0, 0);
    }
}

void VSTPluginDelegate::ref() {
    pluginUseCount_++;
    LOG_DEBUG("ref");
}

void VSTPluginDelegate::unref() {
    pluginUseCount_--;
    if (pluginUseCount_ < 0) {
        LOG_ERROR("BUG: VSTPluginDelegate::unref");
    }
    LOG_DEBUG("unref");
}


/*** unit command callbacks ***/

void vst_open(VSTPlugin *unit, sc_msg_iter *args) {
    const char *path = args->gets();
    auto gui = args->geti();
    if (path) {
        unit->delegate().open(path, gui);
    }
    else {
        LOG_WARNING("vst_open: expecting string argument!");
    }
}

void vst_close(VSTPlugin *unit, sc_msg_iter *args) {
    unit->delegate().close();
}

void vst_reset(VSTPlugin *unit, sc_msg_iter *args) {
    bool async = args->geti();
    unit->delegate().reset(async);
}

void vst_vis(VSTPlugin* unit, sc_msg_iter *args) {
    bool show = args->geti();
    unit->delegate().showEditor(show);
}

// helper function (only call after unit->delegate().check()!)
bool vst_param_index(VSTPlugin* unit, sc_msg_iter *args, int& index) {
    if (args->nextTag() == 's') {
        auto name = args->gets();
        auto& map = unit->delegate().plugin()->info().paramMap;
        auto result = map.find(name);
        if (result != map.end()) {
            index = result->second;
        } else {
            LOG_ERROR("parameter '" << name << "' not found!");
            return false;
        }
    } else {
        index = args->geti();
    }
    return true;
}

// set parameters given as pairs of index and value
void vst_set(VSTPlugin* unit, sc_msg_iter *args) {
    if (unit->delegate().check()) {
        while (args->remain() > 0) {
            int32 index = -1;
            if (vst_param_index(unit, args, index)) {
                if (args->nextTag() == 's') {
                    unit->delegate().setParam(index, args->gets());
                }
                else {
                    unit->delegate().setParam(index, args->getf());
                }
            }
            else {
                args->getf(); // swallow arg
            }
        }
    }
}

// set parameters given as triples of index, count and values
void vst_setn(VSTPlugin* unit, sc_msg_iter *args) {
    if (unit->delegate().check()) {
        while (args->remain() > 0) {
            int32 index = -1;
            if (vst_param_index(unit, args, index)) {
                int32 count = args->geti();
                for (int i = 0; i < count; ++i) {
                    if (args->nextTag() == 's') {
                        unit->delegate().setParam(index + i, args->gets());
                    }
                    else {
                        unit->delegate().setParam(index + i, args->getf());
                    }
                }
            }
            else {
                int32 count = args->geti();
                while (count--) {
                    args->getf(); // swallow args
                }
            }
        }
    }
}

// query parameters starting from index (values + displays)
void vst_param_query(VSTPlugin* unit, sc_msg_iter *args) {
    int32 index = args->geti();
    int32 count = args->geti();
    unit->delegate().queryParams(index, count);
}

// get a single parameter at index (only value)
void vst_get(VSTPlugin* unit, sc_msg_iter *args) {
    int32 index = -1;
    if (vst_param_index(unit, args, index)) {
        unit->delegate().getParam(index);
    }
    else {
        unit->delegate().sendMsg("/vst_set", -1);
    }
}

// get a number of parameters starting from index (only values)
void vst_getn(VSTPlugin* unit, sc_msg_iter *args) {
    int32 index = -1;
    if (vst_param_index(unit, args, index)) {
        int32 count = args->geti();
        unit->delegate().getParams(index, count);
    }
    else {
        unit->delegate().sendMsg("/vst_setn", -1);
    }
}

// map parameters to control busses
void vst_map(VSTPlugin* unit, sc_msg_iter *args) {
    if (unit->delegate().check()) {
        while (args->remain() > 0) {
            int32 index = -1;
            if (vst_param_index(unit, args, index)) {
                int32 bus = args->geti(-1);
                int32 numChannels = args->geti();
                for (int i = 0; i < numChannels; ++i) {
                    unit->delegate().mapParam(index + i, bus + i);
                }
            }
            else {
                args->geti(); // swallow bus
                args->geti(); // swallow numChannels
            }
        }
    }
}

// unmap parameters from control busses
void vst_unmap(VSTPlugin* unit, sc_msg_iter *args) {
    if (unit->delegate().check()) {
        if (args->remain() > 0) {
            do {
                int32 index = -1;
                if (vst_param_index(unit, args, index)) {
                    unit->delegate().unmapParam(index);
                }
            } while (args->remain() > 0);
        }
        else {
            // unmap all parameters:
            int nparam = unit->delegate().plugin()->getNumParameters();
            for (int i = 0; i < nparam; ++i) {
                unit->delegate().unmapParam(i);
            }
        }

    }
}

void vst_program_set(VSTPlugin *unit, sc_msg_iter *args) {
    int32 index = args->geti();
    unit->delegate().setProgram(index);
}

// query parameters (values + displays) starting from index
void vst_program_query(VSTPlugin *unit, sc_msg_iter *args) {
    int32 index = args->geti();
    int32 count = args->geti();
    unit->delegate().queryPrograms(index, count);
}

void vst_program_name(VSTPlugin* unit, sc_msg_iter *args) {
    const char *name = args->gets();
    if (name) {
        unit->delegate().setProgramName(name);
    }
    else {
        LOG_WARNING("vst_program_name: expecting string argument!");
    }
}

void vst_program_read(VSTPlugin* unit, sc_msg_iter *args) {
    if (args->nextTag() == 's') {
        unit->delegate().readPreset<false>(args->gets()); // file name
    }
    else {
        unit->delegate().readPreset<false>(args->geti()); // buf num
    }
}

void vst_program_write(VSTPlugin *unit, sc_msg_iter *args) {
    if (args->nextTag() == 's') {
        unit->delegate().writePreset<false>(args->gets()); // file name
    }
    else {
        unit->delegate().writePreset<false>(args->geti()); // buf num
    }
}

void vst_bank_read(VSTPlugin* unit, sc_msg_iter *args) {
    if (args->nextTag() == 's') {
        unit->delegate().readPreset<true>(args->gets()); // file name
    }
    else {
        unit->delegate().readPreset<true>(args->geti()); // buf num
    }
}

void vst_bank_write(VSTPlugin* unit, sc_msg_iter *args) {
    if (args->nextTag() == 's') {
        unit->delegate().writePreset<true>(args->gets()); // file name
    }
    else {
        unit->delegate().writePreset<true>(args->geti()); // buf num
    }
}

void vst_midi_msg(VSTPlugin* unit, sc_msg_iter *args) {
    char data[4];
    int32 len = args->getbsize();
    if (len > 4) {
        LOG_WARNING("vst_midi_msg: midi message too long (" << len << " bytes)");
    }
    args->getb(data, len);
    unit->delegate().sendMidiMsg(data[0], data[1], data[2]);
}

void vst_midi_sysex(VSTPlugin* unit, sc_msg_iter *args) {
    int len = args->getbsize();
    if (len > 0) {
        // LATER avoid unnecessary copying
        char *buf = (char *)RTAlloc(unit->mWorld, len);
        if (buf) {
            args->getb(buf, len);
            unit->delegate().sendSysexMsg(buf, len);
            RTFree(unit->mWorld, buf);
        }
        else {
            LOG_ERROR("vst_midi_sysex: RTAlloc failed!");
        }
    }
    else {
        LOG_WARNING("vst_midi_sysex: no data!");
    }
}

void vst_tempo(VSTPlugin* unit, sc_msg_iter *args) {
    float bpm = args->getf();
    unit->delegate().setTempo(bpm);
}

void vst_time_sig(VSTPlugin* unit, sc_msg_iter *args) {
    int32 num = args->geti();
    int32 denom = args->geti();
    unit->delegate().setTimeSig(num, denom);
}

void vst_transport_play(VSTPlugin* unit, sc_msg_iter *args) {
    int play = args->geti();
    unit->delegate().setTransportPlaying(play);
}

void vst_transport_set(VSTPlugin* unit, sc_msg_iter *args) {
    float pos = args->getf();
    unit->delegate().setTransportPos(pos);
}

void vst_transport_get(VSTPlugin* unit, sc_msg_iter *args) {
    unit->delegate().getTransportPos();
}

void vst_can_do(VSTPlugin* unit, sc_msg_iter *args) {
    const char* what = args->gets();
    if (what) {
        unit->delegate().canDo(what);
    }
}

void vst_vendor_method(VSTPlugin* unit, sc_msg_iter *args) {
    int32 index = args->geti();
    int32 value = args->geti(); // sc_msg_iter doesn't support 64bit ints...
    int32 size = args->getbsize();
    char *data = nullptr;
    if (size > 0) {
        data = (char *)RTAlloc(unit->mWorld, size);
        if (data) {
            args->getb(data, size);
        }
        else {
            LOG_ERROR("RTAlloc failed!");
            return;
        }
    }
    float opt = args->getf();
    bool async = args->geti();
    unit->delegate().vendorSpecific(index, value, size, data, opt, async);
    if (data) {
        RTFree(unit->mWorld, data);
    }
}

/*** plugin command callbacks ***/

// recursively search directories for VST plugins.
bool cmdSearch(World *inWorld, void* cmdData) {
    auto data = (InfoCmdData *)cmdData;
    std::vector<PluginInfo::const_ptr> plugins;
    bool useDefault = data->flags & SearchFlags::useDefault;
    bool verbose = data->flags & SearchFlags::verbose;
    bool save = data->flags & SearchFlags::save;
    bool parallel = data->flags & SearchFlags::parallel;
    std::vector<std::string> searchPaths;
    auto size = data->size;
    auto ptr = data->buf;
    auto onset = ptr;
    // file paths are seperated by 0
    while (size--) {
        if (*ptr++ == '\0') {
            auto diff = ptr - onset;
            searchPaths.emplace_back(onset, diff - 1); // don't store '\0'!
            onset = ptr;
        }
    }
    // use default search paths?
    if (useDefault) {
        for (auto& path : getDefaultSearchPaths()) {
            searchPaths.push_back(path);
        }
    }
    // search for plugins
    for (auto& path : searchPaths) {
        auto result = searchPlugins(path, parallel, verbose);
        plugins.insert(plugins.end(), result.begin(), result.end());
    }
    if (save){
        writeIniFile();
    }
    // write new info to file (only for local Servers) or buffer
    if (data->path[0]) {
        // write to file
        std::ofstream file(data->path, std::ios_base::binary | std::ios_base::trunc);
        if (file.is_open()) {
            LOG_DEBUG("writing plugin info to file");
            file << "[plugins]\n";
            file << "n=" << plugins.size() << "\n";
            for (auto& plugin : plugins) {
                serializePlugin(file, *plugin);
            }
        }
        else {
            LOG_ERROR("couldn't write plugin info file '" << data->path << "'!");
        }
    }
    else if (data->bufnum >= 0) {
        // write to buffer
        auto buf = World_GetNRTBuf(inWorld, data->bufnum);
        data->freeData = buf->data; // to be freed in stage 4
        std::stringstream ss;
        LOG_DEBUG("writing plugin info to buffer");
        ss << "[plugins]\n";
        ss << "n=" << plugins.size() << "\n";
        for (auto& plugin : plugins) {
            serializePlugin(ss, *plugin);
        }
        allocReadBuffer(buf, ss.str());
    }
    // else do nothing

    return true;
}

bool cmdSearchDone(World *inWorld, void *cmdData) {
    auto data = (InfoCmdData*)cmdData;
    if (data->bufnum >= 0)
        syncBuffer(inWorld, data->bufnum);
    gSearching = false;
    // LOG_DEBUG("search done!");
    return true;
}

void vst_search(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
    if (gSearching) {
        LOG_WARNING("already searching!");
        return;
    }
    int32 bufnum = -1;
    const char* filename = nullptr;
    // flags (useDefault, verbose, etc.)
    int flags = args->geti();
    // temp file or buffer to store the search results
    if (args->nextTag() == 's') {
        filename = args->gets();
    }
    else {
        bufnum = args->geti();
        if (bufnum >= inWorld->mNumSndBufs) {
            LOG_ERROR("vst_search: bufnum " << bufnum << " out of range");
            return;
        }
    }
    // collect optional search paths
    std::pair<const char *, size_t> paths[64];
    int numPaths = 0;
    auto pathLen = 0;
    while (args->remain() && numPaths < 64) {
        auto s = args->gets();
        if (s) {
            auto len = strlen(s) + 1; // include terminating '\0'
            paths[numPaths].first = s;
            paths[numPaths].second = len;
            pathLen += len;
            ++numPaths;
        }
    }

    auto data = CmdData::create<InfoCmdData>(inWorld, pathLen);
    if (data) {
        data->flags = flags;
        data->bufnum = bufnum; // negative bufnum: don't write search result
        if (filename) {
            snprintf(data->path, sizeof(data->path), "%s", filename);
        }
        else {
            data->path[0] = '\0';
        }
        // now copy search paths into a single buffer (separated by '\0')
        data->size = pathLen;
        auto ptr = data->buf;
        for (int i = 0; i < numPaths; ++i) {
            auto s = paths[i].first;
            auto len = paths[i].second;
            memcpy(ptr, s, len);
            ptr += len;
        }
        // LOG_DEBUG("start search");
        DoAsynchronousCommand(inWorld, replyAddr, "vst_search",
            data, cmdSearch, cmdSearchDone, InfoCmdData::nrtFree, cmdRTfree, 0, 0);
        gSearching = true;
    }
}

void vst_clear(World* inWorld, void* inUserData, struct sc_msg_iter* args, void* replyAddr) {
    if (!gSearching) {
        auto data = CmdData::create<InfoCmdData>(inWorld);
        if (data) {
            data->flags = args->geti(); // 1 = remove cache file
            DoAsynchronousCommand(inWorld, replyAddr, "vst_clear", data, [](World*, void* data) {
                gPluginManager.clear();
                int flags = static_cast<InfoCmdData*>(data)->flags;
                if (flags & 1) {
                    // remove cache file
                    removeFile(getSettingsDir() + "/" SETTINGS_FILE);
                }
                return false;
            }, 0, 0, cmdRTfree, 0, 0);
        }
    }
    else {
        LOG_WARNING("can't clear while searching!");
    }
}

// query plugin info
bool cmdProbe(World *inWorld, void *cmdData) {
    auto data = (InfoCmdData *)cmdData;
    auto desc = queryPlugin(data->buf);
    // write info to file or buffer
    if (desc){
        if (data->path[0]) {
            // write to file
            LOG_DEBUG("writing plugin info to file");
            std::ofstream file(data->path, std::ios_base::binary | std::ios_base::trunc);
            if (file.is_open()) {
                serializePlugin(file, *desc);
            }
            else {
                LOG_ERROR("couldn't write plugin info file '" << data->path << "'!");
            }
        }
        else if (data->bufnum >= 0) {
            // write to buffer
            auto buf = World_GetNRTBuf(inWorld, data->bufnum);
            data->freeData = buf->data; // to be freed in stage 4
            std::stringstream ss;
            LOG_DEBUG("writing plugin info to buffer");
            serializePlugin(ss, *desc);
            allocReadBuffer(buf, ss.str());
        }
        // else do nothing
        return true;
    }
    return false;
}

bool cmdProbeDone(World* inWorld, void* cmdData) {
    auto data = (InfoCmdData*)cmdData;
    if (data->bufnum >= 0)
        syncBuffer(inWorld, data->bufnum);
    // LOG_DEBUG("probe done!");
    return true;
}

void vst_probe(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
    if (gSearching) {
        LOG_WARNING("currently searching!");
        return;
    }
    if (args->nextTag() != 's') {
        LOG_ERROR("first argument to 'vst_probe' must be a string (plugin path)!");
        return;
    }
    auto path = args->gets(); // plugin path
    auto size = strlen(path) + 1;

    auto data = CmdData::create<InfoCmdData>(inWorld, size);
    if (data) {
        // temp file or buffer to store the plugin info
        if (args->nextTag() == 's') {
            auto filename = args->gets();
            snprintf(data->path, sizeof(data->path), "%s", filename);
        }
        else {
            auto bufnum = args->geti();
            if (bufnum >= inWorld->mNumSndBufs) {
                LOG_ERROR("vst_search: bufnum " << bufnum << " out of range");
                return;
            }
            data->bufnum = bufnum; // negative bufnum: don't write probe result
            data->path[0] = '\0';
        }

        memcpy(data->buf, path, size); // store plugin path

        DoAsynchronousCommand(inWorld, replyAddr, "vst_probe",
            data, cmdProbe, cmdProbeDone, InfoCmdData::nrtFree, cmdRTfree, 0, 0);
    }
}

/*** plugin entry point ***/

void VSTPlugin_Ctor(VSTPlugin* unit){
    new(unit)VSTPlugin();
}

void VSTPlugin_Dtor(VSTPlugin* unit){
    unit->~VSTPlugin();
}

using VSTUnitCmdFunc = void (*)(VSTPlugin*, sc_msg_iter*);

// When a Synth is created on the Server, the UGen constructors are only called during
// the first "next" routine, so if we send a unit command right after /s_new, the receiving
// unit hasn't been properly constructed yet, so calling member functions might lead to a crash.

// The previous version of VSTPlugin just ignored such unit commands and posted a warning,
// now we queue them and run them in the constructor.

// In RT synthesis this is most useful for opening plugins right after Synth creation, e.g.:
// VSTPluginController(Synth(\test)).open("some_plugin", action: { |plugin| /* do something */ });

// In NRT synthesis this becomes even more useful because all commands are executed synchronously,
// so you can schedule /s_new + various unit commands (e.g. openMsg -> readProgramMsg) for the same timestamp.

// Unit commands likely trigger ansynchronous commands - which is not a problem in Scsynth.
// In Supernova there's a theoretical race condition issue since the system FIFO is single producer only,
// but UGen constructors never run in parallel, so this is safe as long as nobody else is scheduling
// system callbacks during the "next" routine (which would be dangerous anyway).

// Another problem is that the Server doesn't zero any RT memory for performance reasons.
// This means we can't check for 0 or nullptrs... The current solution is to set the
// "initialized_" member to some magic value in the constructor. In the destructor we zero the
// field to protected against cases where the next VSTPlugin instance we be allocated at the same address.
// The member has to be volate to ensure that the compiler doesn't eliminate any stores!
template<VSTUnitCmdFunc fn>
void runUnitCmd(VSTPlugin* unit, sc_msg_iter* args) {
    if (unit->initialized()) {
        fn(unit, args); // the constructor has been called, so we can savely run the command
    } else {
        unit->queueUnitCmd((UnitCmdFunc)fn, args); // queue it
    }
}

#define UnitCmd(x) DefineUnitCmd("VSTPlugin", "/" #x, (UnitCmdFunc)runUnitCmd<vst_##x>)
#define PluginCmd(x) DefinePlugInCmd("/" #x, x, 0)

PluginLoad(VSTPlugin) {
    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable
    rt::interfaceTable = inTable; // for "rt_shared_ptr.h"

    DefineDtorCantAliasUnit(VSTPlugin);
    UnitCmd(open);
    UnitCmd(close);
    UnitCmd(reset);
    UnitCmd(vis);
    UnitCmd(set);
    UnitCmd(setn);
    UnitCmd(param_query);
    UnitCmd(get);
    UnitCmd(getn);
    UnitCmd(map);
    UnitCmd(unmap);
    UnitCmd(program_set);
    UnitCmd(program_query);
    UnitCmd(program_name);
    UnitCmd(program_read);
    UnitCmd(program_write);
    UnitCmd(bank_read);
    UnitCmd(bank_write);
    UnitCmd(midi_msg);
    UnitCmd(midi_sysex);
    UnitCmd(tempo);
    UnitCmd(time_sig);
    UnitCmd(transport_play);
    UnitCmd(transport_set);
    UnitCmd(transport_get);
    UnitCmd(can_do);
    UnitCmd(vendor_method);

    PluginCmd(vst_search);
    PluginCmd(vst_clear);
    PluginCmd(vst_probe);

    // read cached plugin info
    readIniFile();
}


