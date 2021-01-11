#include "VSTPlugin.h"

#ifdef SUPERNOVA
#include <nova-tt/spin_lock.hpp>
#include <nova-tt/rw_spinlock.hpp>
#endif

static InterfaceTable *ft;

namespace rt {
    InterfaceTable* interfaceTable;
}

void SCLog(const char *s){
    Print("%s", s);
}

void RTFreeSafe(World *world, void *data){
    if (data){
        RTFree(world, data);
    }
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

PresetCmdData* PresetCmdData::create(World* world, const char* path, bool async) {
    auto len = strlen(path) + 1;
    auto data = CmdData::create<PresetCmdData>(world, len);
    if (data) {
        data->bufnum = -1;
        memcpy(data->path, path, len);
        data->async = async;
    }
    return data;
}

PresetCmdData* PresetCmdData::create(World* world, int bufnum, bool async) {
    auto data = CmdData::create<PresetCmdData>(world);
    if (data) {
        data->bufnum = bufnum;
        data->path[0] = '\0';
        data->async = async;
    }
    return data;
}

bool PresetCmdData::nrtFree(World* inWorld, void* cmdData) {
    auto data = (PresetCmdData*)cmdData;
    // this is potentially dangerous because NRTFree internally uses free()
    // while BufFreeCmd::Stage4() uses free_aligned().
    // On the other hand, the client is supposed to pass an *unused* bufnum,
    // so ideally we don't have to free any previous data.
    // The SndBuf is then freed by the client.
    if (data->freeData)
        NRTFree(data->freeData);
    // free preset data
    std::string dummy;
    std::swap(data->buffer, dummy);
    return true;
}

bool SearchCmdData::nrtFree(World *world, void *cmdData){
    // see PresetCmdData::nrtFree
    auto data = (SearchCmdData*)cmdData;
    if (data->freeData)
        NRTFree(data->freeData);
    return true;
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

template<typename T>
void defer(const T& fn, bool uithread){
    // defer function call to the correct thread
    if (uithread){
        bool result;
        Error err;
        bool ok = UIThread::callSync([&](){
            try {
                fn();
                result = true;
            } catch (const Error& e){
                err = e;
                result = false;
            }
        });
        if (ok){
            if (!result){
                throw err;
            }
            return;
        } else {
            LOG_ERROR("UIThread::callSync() failed");
        }
    }
    // call on this thread
    fn();
}

// search and probe
static std::atomic_bool gSearching {false};

static PluginManager gPluginManager;

#define SETTINGS_DIR ".VSTPlugin"
// so that 64-bit and 32-bit installations can co-exist!
#if (defined(_WIN32) && !defined(_WIN64)) || defined(__i386__)
#define CACHE_FILE "cache32.ini"
#else
#define CACHE_FILE "cache.ini"
#endif

static std::string getSettingsDir(){
#ifdef _WIN32
    return expandPath("%USERPROFILE%\\" SETTINGS_DIR);
#else
    return expandPath("~/" SETTINGS_DIR);
#endif
}

static Mutex gFileLock;

static void readIniFile(){
    ScopedLock lock(gFileLock);
    auto path = getSettingsDir() + "/" CACHE_FILE;
    if (pathExists(path)){
        LOG_VERBOSE("read cache file " << path.c_str());
        try {
            gPluginManager.read(path);
        } catch (const Error& e){
            LOG_ERROR("ERROR: couldn't read cache file: " << e.what());
        }
    }
}

static void writeIniFile(){
    ScopedLock lock(gFileLock);
    try {
        auto dir = getSettingsDir();
        if (!pathExists(dir)){
            if (!createDirectory(dir)){
                throw Error("couldn't create directory " + dir);
            }
        }
        gPluginManager.write(dir + "/" CACHE_FILE);
    } catch (const Error& e){
        LOG_ERROR("couldn't write cache file: " << e.what());
    }
}

// VST2: plug-in name
// VST3: plug-in name + ".vst3"
static std::string makeKey(const PluginInfo& desc) {
    if (desc.type() == PluginType::VST3){
        return desc.name + ".vst3";
    } else {
        return desc.name;
    }
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
        // always print error
        Print("ERROR: couldn't load '%s': %s\n", path.c_str(), e.what());
        gPluginManager.addException(path);
        return nullptr;
    }

    return factory;
}

static void addFactory(const std::string& path, IFactory::ptr factory){
    if (factory->numPlugins() == 1) {
        auto plugin = factory->getPlugin(0);
        // factories with a single plugin can also be aliased by their file path(s)
        gPluginManager.addPlugin(plugin->path(), plugin);
        gPluginManager.addPlugin(path, plugin);
    }
    gPluginManager.addFactory(path, factory);
    for (int i = 0; i < factory->numPlugins(); ++i) {
        auto plugin = factory->getPlugin(i);
    #if 0
        // search for presets
        const_cast<PluginInfo&>(*plugin).scanPresets();
    #endif
        gPluginManager.addPlugin(makeKey(*plugin), plugin);
    }
}

static void postResult(const Error& e){
    switch (e.code()) {
    case Error::NoError:
        Print("ok!\n");
        break;
    case Error::Crash:
        Print("crashed!\n");
        break;
    case Error::SystemError:
        Print("error! %s\n", e.what());
        break;
    case Error::ModuleError:
        Print("couldn't load! %s\n", e.what());
        break;
    case Error::PluginError:
        Print("failed! %s\n", e.what());
        break;
    default:
        Print("unexpected error! %s\n", e.what());
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
        factory->probe([&](const ProbeResult& result) {
            if (verbose) {
                if (result.total > 1) {
                    if (result.index == 0) {
                        Print("\n");
                    }
                    Print("\t[%d/%d] ", result.index + 1, result.total);
                    if (result.plugin && !result.plugin->name.empty()) {
                        Print("'%s' ... ", result.plugin->name.c_str());
                    } else {
                        Print("... ");
                    }
                }
                postResult(result.error);
            }
        });
        if (factory->valid()){
            addFactory(path, factory);
            return factory; // success
        }
    } catch (const Error& e){
        if (verbose) postResult(e);
    }
    gPluginManager.addException(path);
    return nullptr;
}

using FactoryFuture = std::function<bool(IFactory::ptr&)>;

static FactoryFuture probePluginAsync(const std::string& path, bool verbose) {
    auto factory = loadFactory(path, verbose);
    if (!factory){
        return [](IFactory::ptr& out) {
            out = nullptr;
            return true;
        };
    }
    // start probing process
    try {
        auto future = factory->probeAsync(true);
        // return future
        return [=](IFactory::ptr& out) {
            // wait for results
            bool done = future([&](const ProbeResult& result) {
                if (verbose) {
                    if (result.total > 1) {
                        // several subplugins
                        if (result.index == 0) {
                            Print("probing %s... \n", path.c_str());
                        }
                        Print("\t[%d/%d] ", result.index + 1, result.total);
                        if (result.plugin && !result.plugin->name.empty()) {
                            Print("'%s' ... ", result.plugin->name.c_str());
                        } else {
                            Print("... ");
                        }
                    } else {
                        // single plugin
                        Print("probing %s... ", path.c_str());
                    }
                    postResult(result.error);
                }
            });

            if (done){
                // collect results
                if (factory->valid()){
                    addFactory(path, factory);
                    out = factory; // success
                } else {
                    gPluginManager.addException(path);
                    out = nullptr;
                }
                return true;
            } else {
                return false;
            }
        };
    }
    catch (const Error& e) {
        // return future which prints the error message
        return [=](IFactory::ptr& out) {
            if (verbose) {
                Print("probing %s... ", path.c_str());
                postResult(e);
            }
            gPluginManager.addException(path);
            out = nullptr;
            return true;
        };
    }
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

#define PROBE_FUTURES 8

std::vector<PluginInfo::const_ptr> searchPlugins(const std::string & path,
                                                 bool parallel, bool verbose) {
    Print("searching in '%s'...\n", path.c_str());
    std::vector<PluginInfo::const_ptr> results;

    auto addPlugin = [&](PluginInfo::const_ptr plugin, int which = 0, int n = 0){
        if (verbose && n > 0) {
            Print("\t[%d/%d] %s\n", which + 1, n, plugin->name.c_str());
        }
        results.push_back(plugin);
    };

    std::vector<FactoryFuture> futures;

    auto processFutures = [&](int limit){
        while (futures.size() > limit){
            for (auto it = futures.begin(); it != futures.end(); ){
                IFactory::ptr factory;
                if ((*it)(factory)){
                    // future finished
                    if (factory){
                        for (int i = 0; i < factory->numPlugins(); ++i){
                            addPlugin(factory->getPlugin(i));
                        }
                    }
                    // remove future
                    it = futures.erase(it);
                } else {
                    it++;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    vst::search(path, [&](const std::string & absPath) {
        if (!gSearching){
            return;
        }
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
                for (int i = 0; i < numPlugins; ++i) {
                    // add and post plugins
                    addPlugin(factory->getPlugin(i), i, numPlugins);
                }
            }
        }
        else {
            // probe (will post results and add plugins)
            if (parallel){
                futures.push_back(probePluginAsync(pluginPath, verbose));
                processFutures(PROBE_FUTURES);
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
    processFutures(0);

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
    // UGen inputs: nout, flags, bypass, nin, inputs..., nauxin, auxinputs..., nparam, params...
    assert(numInputs() >= 5);
    // out
    int nout = numOutChannels();
    LOG_DEBUG("out: " << nout);
    assert(!(nout < 0 || nout > numOutputs()));
    // in
    auto nin = numInChannels();
    LOG_DEBUG("in: " << nin);
    assert(nin >= 0);
    // aux in
    auxInChannelOnset_ = inChannelOnset_ + nin + 1;
    assert(auxInChannelOnset_ > inChannelOnset_ && auxInChannelOnset_ < numInputs());
    auto nauxin = numAuxInChannels(); // computed from auxInChannelsOnset_
    LOG_DEBUG("aux in: " << nauxin);
    assert(nauxin >= 0);
    // aux out
    auto nauxout = numAuxOutChannels();
    // parameter controls
    parameterControlOnset_ = auxInChannelOnset_ + nauxin + 1;
    assert(parameterControlOnset_ > auxInChannelOnset_ && parameterControlOnset_ <= numInputs());
    auto numParams = numParameterControls(); // computed from parameterControlsOnset_
    LOG_DEBUG("parameters: " << numParams);
    assert(numParams >= 0);
    assert((parameterControlOnset_ + numParams * 2) <= numInputs());
 
    // create delegate after member initialization!
    delegate_ = rt::make_shared<VSTPluginDelegate>(mWorld, *this);

    set_calc_function<VSTPlugin, &VSTPlugin::next>();

    // create reblocker after setting the calc function (to avoid 1 sample computation)!
    int reblockSize = in0(1);
    if (reblockSize > bufferSize()){
        auto reblock = (Reblock *)RTAlloc(mWorld, sizeof(Reblock));
        if (reblock){
            memset(reblock, 0, sizeof(Reblock)); // set all pointers to NULL!
            // make sure that block size is power of 2
            int n = 1;
            while (n < reblockSize){
               n *= 2;
            }
            reblock->blockSize = n;
            // allocate buffer
            int total = nin + nout + nauxin + nauxout;
            if ((total == 0) ||
                (reblock->buffer = (float *)RTAlloc(mWorld, sizeof(float) * total * n)))
            {
                auto bufptr = reblock->buffer;
                std::fill(bufptr, bufptr + (total * n), 0);
                // allocate and assign channel vectors
                auto makeVec = [&](auto& vec, int nchannels){
                    if (nchannels > 0){
                        vec = (float **)RTAlloc(mWorld, sizeof(float *) * nchannels);
                        if (!vec){
                            LOG_ERROR("RTAlloc failed!");
                            return false;
                        }
                        for (int i = 0; i < nchannels; ++i, bufptr += n){
                            vec[i] = bufptr;
                        }
                    }
                    return true;
                };
                if (makeVec(reblock->input, nin)
                    && makeVec(reblock->output, nout)
                    && makeVec(reblock->auxInput, nauxin)
                    && makeVec(reblock->auxOutput, nauxout))
                {
                    // start phase at one block before end, so that the first call
                    // to the perform routine will trigger plugin processing.
                    reblock->phase = n - bufferSize();
                    reblock_ = reblock;
                } else {
                    // clean up
                    RTFreeSafe(mWorld, reblock->input);
                    RTFreeSafe(mWorld, reblock->output);
                    RTFreeSafe(mWorld, reblock->auxInput);
                    RTFreeSafe(mWorld, reblock->auxOutput);
                    RTFreeSafe(mWorld, reblock->buffer);
                    RTFree(mWorld, reblock);
                }
            } else {
                LOG_ERROR("RTAlloc failed!");
                RTFree(mWorld, reblock);
            }
        } else {
            LOG_ERROR("RTAlloc failed!");
        }
    }

    // run queued unit commands
    if (mSpecialIndex & UnitCmdQueued) {
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

    mSpecialIndex |= Initialized; // !

    LOG_DEBUG("created VSTPlugin instance");
}

VSTPlugin::~VSTPlugin(){
    clearMapping();
    RTFreeSafe(mWorld, paramState_);
    RTFreeSafe(mWorld, paramMapping_);
    if (reblock_){
        RTFreeSafe(mWorld, reblock_->input);
        RTFreeSafe(mWorld, reblock_->output);
        RTFreeSafe(mWorld, reblock_->auxInput);
        RTFreeSafe(mWorld, reblock_->auxOutput);
        RTFreeSafe(mWorld, reblock_->buffer);
        RTFree(mWorld, reblock_);
    }
    // tell the delegate that we've been destroyed!
    delegate_->setOwner(nullptr);
    delegate_ = nullptr; // release our reference
    LOG_DEBUG("destroyed VSTPlugin");
}

// HACK to check if the class has been fully constructed. See "runUnitCommand".
bool VSTPlugin::initialized() {
    return (mSpecialIndex & Initialized);
}

// Terrible hack to enable sending unit commands right after /s_new
// although the UGen constructor hasn't been called yet. See "runUnitCommand".
void VSTPlugin::queueUnitCmd(UnitCmdFunc fn, sc_msg_iter* args) {
    if (!(mSpecialIndex & UnitCmdQueued)) {
        unitCmdQueue_ = nullptr;
        mSpecialIndex |= UnitCmdQueued;
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

void VSTPlugin::clearMapping() {
    for (auto m = paramMappingList_; m != nullptr; ){
        paramMapping_[m->index] = nullptr;
        auto next = m->next;
        RTFree(mWorld, m);
        m = next;
    }
    paramMappingList_ = nullptr;
}

float VSTPlugin::readControlBus(uint32 num) {
    if (num < mWorld->mNumControlBusChannels) {
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
    paramQueue_.clear();
    clearMapping();
    int n = delegate().plugin()->info().numParameters();
    // parameter states
    {
        float* result = nullptr;
        if (n > 0) {
            // Scsynth's RTRealloc doesn't return nullptr for size == 0
            result = (float*)RTRealloc(mWorld, paramState_, n * sizeof(float));
            if (!result) {
                LOG_ERROR("RTRealloc failed!");
            }
        }
        if (result) {
            for (int i = 0; i < n; ++i) {
            #if 0
                // breaks floating point comparison on GCC with -ffast-math
                result[i] = std::numeric_limits<float>::quiet_NaN();
            #else
                result[i] = std::numeric_limits<float>::max();
            #endif
            }
            paramState_ = result;
        } else {
            RTFreeSafe(mWorld, paramState_);
            paramState_ = nullptr;
        }
    }
    // parameter mapping
    {
        Mapping** result = nullptr;
        if (n > 0) {
            result = (Mapping **)RTRealloc(mWorld, paramMapping_, n * sizeof(Mapping*));
            if (!result) {
                LOG_ERROR("RTRealloc failed!");
            }
        }
        if (result) {
            for (int i = 0; i < n; ++i) {
                result[i] = nullptr;
            }
            paramMapping_ = result;
        } else {
            RTFreeSafe(mWorld, paramMapping_);
            paramMapping_ = nullptr;
        }
    }
}

#if 1
#define PRINT_MAPPING \
    LOG_DEBUG("mappings:"); \
    for (auto mapping = paramMappingList_; mapping; mapping = mapping->next){ \
        LOG_DEBUG(mapping->index << " -> " << mapping->bus() << " (" << mapping->type() << ")"); \
    }
#else
#define PRINT_MAPPING
#endif

void VSTPlugin::map(int32 index, int32 bus, bool audio) {
    Mapping *mapping = paramMapping_[index];
    if (mapping == nullptr) {
        mapping = (Mapping*)RTAlloc(mWorld, sizeof(Mapping));
        if (mapping) {
            // add to head of linked list
            mapping->index = index;
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
            return;
        }
    }
    mapping->setBus(bus, audio ? Mapping::Audio : Mapping::Control);
    PRINT_MAPPING
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
    PRINT_MAPPING
}

// perform routine
void VSTPlugin::next(int inNumSamples) {
#ifdef SUPERNOVA
    // for Supernova the "next" routine might be called in a different thread - each time!
    delegate_->rtThreadID_ = std::this_thread::get_id();
#endif
    auto plugin = delegate_->plugin();
    bool process = plugin && plugin->info().hasPrecision(ProcessPrecision::Single);
    bool suspended = delegate_->suspended();
    if (process && suspended){
        // Whenever an asynchronous command is executing, the plugin is temporarily
        // suspended. This is mainly for blocking other commands until the async
        // command has finished. The actual critical section is protected by a mutex.
        // We use tryLock() and bypass on failure, so we don't block the whole Server.
        process = delegate_->tryLock();
        if (!process){
            LOG_DEBUG("couldn't lock mutex");
        }
    }

    if (process) {
        auto vst3 = plugin->info().type() == PluginType::VST3;

        // check bypass state
        Bypass bypass = Bypass::Off;
        int inBypass = getBypass();
        if (inBypass > 0) {
            if (inBypass == 1) {
                bypass = Bypass::Hard;
            }
            else {
                bypass = Bypass::Soft;
            }
        }
        if (bypass != bypass_) {
            plugin->setBypass(bypass);
            bypass_ = bypass;
        }

        // parameter automation
        // (check paramState_ in case RTAlloc failed)
        if (paramState_) {
            // automate parameters with mapped control busses
            int nparams = plugin->info().numParameters();
            for (auto m = paramMappingList_; m != nullptr; m = m->next) {
                uint32 index = m->index;
                auto type = m->type();
                uint32 num = m->bus();
                assert(index < nparams);
                // Control Bus mapping
                if (type == Mapping::Control) {
                    float value = readControlBus(num);
                    if (value != paramState_[index]) {
                        plugin->setParameter(index, value);
                        paramState_[index] = value;
                    }
                }
                // Audio Bus mapping
                else if (num < mWorld->mNumAudioBusChannels){
                #define unit this
                    float last = paramState_[index];
                    float* bus = &mWorld->mAudioBus[mWorld->mBufLength * num];
                    ACQUIRE_BUS_AUDIO_SHARED(num);
                    // VST3: sample accurate
                    if (vst3) {
                        for (int i = 0; i < inNumSamples; ++i) {
                            float value = bus[i];
                            if (value != last) {
                                plugin->setParameter(index, value, i); // sample offset!
                                last = value;
                            }
                        }
                    }
                    // VST2: pick the first sample
                    else {
                        float value = *bus;
                        if (value != last) {
                            plugin->setParameter(index, value);
                            last = value;
                        }
                    }
                    RELEASE_BUS_AUDIO_SHARED(num);
                    paramState_[index] = last;
                #undef unit
                }
            }
            // automate parameters with UGen inputs
            auto numControls = numParameterControls();
            for (int i = 0; i < numControls; ++i) {
                int k = 2 * i + parameterControlOnset_;
                int index = in0(k);
                // only if index is not out of range and the parameter is not mapped to a bus
                // (a negative index effectively deactivates the parameter control)
                if (index >= 0 && index < nparams && paramMapping_[index] == nullptr){
                    auto calcRate = mInput[k + 1]->mCalcRate;
                    // audio rate
                    if (calcRate == calc_FullRate) {
                        float last = paramState_[index];
                        auto buf = in(k + 1);
                        // VST3: sample accurate
                        if (vst3) {
                            for (int i = 0; i < inNumSamples; ++i) {
                                float value = buf[i];
                                if (value != last) {
                                    plugin->setParameter(index, value, i); // sample offset!
                                    last = value;
                                }
                            }
                        }
                        // VST2: pick the first sample
                        else {
                            float value = *buf;
                            if (value != last) {
                                plugin->setParameter(index, value);
                                last = value;
                            }
                        }
                        paramState_[index] = last;
                    }
                    // control rate
                    else {
                        float value = in0(k + 1);
                        if (value != paramState_[index]) {
                            plugin->setParameter(index, value);
                            paramState_[index] = value;
                        }
                    }
                }
            }
        }
        // process
        IPlugin::ProcessData<float> data;

        if (reblock_){
            auto readInput = [](float ** from, float ** to, int nchannels, int nsamples, int phase){
                for (int i = 0; i < nchannels; ++i){
                    std::copy(from[i], from[i] + nsamples, to[i] + phase);
                }
            };
            auto writeOutput = [](float ** from, float ** to, int nchannels, int nsamples, int phase){
                for (int i = 0; i < nchannels; ++i){
                    auto src = from[i] + phase;
                    std::copy(src, src + nsamples, to[i]);
                }
            };

            data.numInputs = numInChannels();
            data.numOutputs = numOutChannels();
            data.numAuxInputs = numAuxInChannels();
            data.numAuxOutputs = numAuxOutChannels();

            readInput(mInBuf + inChannelOnset_, reblock_->input, data.numInputs,
                        inNumSamples, reblock_->phase);
            readInput(mInBuf + auxInChannelOnset_, reblock_->auxInput, data.numAuxInputs,
                        inNumSamples, reblock_->phase);

            reblock_->phase += inNumSamples;

            if (reblock_->phase >= reblock_->blockSize){
                assert(reblock_->phase == reblock_->blockSize);
                reblock_->phase = 0;

                data.input = data.numInputs > 0 ? (const float **)reblock_->input : nullptr;
                data.output = data.numOutputs > 0 ? reblock_->output : nullptr;
                data.auxInput = data.numAuxInputs > 0 ? (const float **)reblock_->auxInput : nullptr;
                data.auxOutput = data.numAuxOutputs > 0 ? reblock_->auxOutput : nullptr;
                data.numSamples = reblock_->blockSize;

                plugin->process(data);
            }

            writeOutput(reblock_->output, mOutBuf, data.numOutputs,
                        inNumSamples, reblock_->phase);
            writeOutput(reblock_->auxOutput, mOutBuf + numOutChannels(), data.numAuxOutputs,
                        inNumSamples, reblock_->phase);
        } else {
            data.numInputs = numInChannels();
            data.input = data.numInputs > 0 ? (const float **)(mInBuf + inChannelOnset_) : nullptr;
            data.numOutputs = numOutChannels();
            data.output = data.numOutputs > 0 ? mOutBuf : nullptr;
            data.numAuxInputs = numAuxInChannels();
            data.auxInput = data.numAuxInputs > 0 ? (const float **)(mInBuf + auxInChannelOnset_) : nullptr;
            data.numAuxOutputs = numAuxOutChannels();
            data.auxOutput = data.numAuxOutputs > 0 ? mOutBuf + numOutChannels() : nullptr;
            data.numSamples = inNumSamples;

            plugin->process(data);
        }

        // send parameter automation notification posted from the GUI thread [or NRT thread]
        ParamChange p;
        while (paramQueue_.pop(p)){
            if (p.index >= 0){
                delegate_->sendParameterAutomated(p.index, p.value);
            } else if (p.index == VSTPluginDelegate::LatencyChange){
                delegate_->sendLatencyChange(p.value);
            } else if (p.index == VSTPluginDelegate::PluginCrash){
                delegate_->sendPluginCrash();
            }
        }
        if (suspended){
            delegate_->unlock();
        }
    } else {
        // bypass (copy input to output and zero remaining output channels)
        auto doBypass = [](auto input, int nin, auto output, int nout, int n, int phase) {
            for (int i = 0; i < nout; ++i) {
                if (i < nin) {
                    auto src = input[i] + phase;
                    std::copy(src, src + n, output[i]);
                }
                else {
                    std::fill(output[i], output[i] + n, 0); // zero
                }
            }
        };

        if (reblock_){
            // we have to copy the inputs to the reblocker, so that we
            // can stop bypassing anytime and always have valid input data.
            auto readInput = [](float ** from, float ** to, int nchannels, int nsamples, int phase){
                for (int i = 0; i < nchannels; ++i){
                    std::copy(from[i], from[i] + nsamples, to[i] + phase);
                }
            };

            readInput(mInBuf + inChannelOnset_, reblock_->input, numInChannels(),
                      inNumSamples, reblock_->phase);
            readInput(mInBuf + auxInChannelOnset_, reblock_->auxInput, numAuxInChannels(),
                      inNumSamples, reblock_->phase);

            // advance phase before bypassing to keep delay!
            reblock_->phase += inNumSamples;

            if (reblock_->phase >= reblock_->blockSize){
                assert(reblock_->phase == reblock_->blockSize);
                reblock_->phase = 0;
            }

            // input -> output
            doBypass(reblock_->input, numInChannels(), mOutBuf, numOutChannels(),
                     inNumSamples, reblock_->phase);
            // aux input -> aux output
            doBypass(reblock_->auxInput, numAuxInChannels(), mOutBuf + numOutChannels(),
                     numAuxOutChannels(), inNumSamples, reblock_->phase);
        } else {
            // input -> output
            doBypass(mInBuf + inChannelOnset_, numInChannels(), mOutBuf, numOutChannels(),
                     inNumSamples, 0);
            // aux input -> aux output
            doBypass(mInBuf + auxInChannelOnset_, numAuxInChannels(), mOutBuf + numOutChannels(),
                     numAuxOutChannels(), inNumSamples, 0);
        }
    }
}

int VSTPlugin::blockSize() const {
    return reblock_ ? reblock_->blockSize : bufferSize();
}

//------------------- VSTPluginDelegate ------------------------------//

VSTPluginDelegate::VSTPluginDelegate(VSTPlugin& owner) {
    setOwner(&owner);
    rtThreadID_ = std::this_thread::get_id();
    // LOG_DEBUG("RT thread ID: " << rtThreadID_);
}

VSTPluginDelegate::~VSTPluginDelegate() {
    close();
    LOG_DEBUG("VSTPluginDelegate destroyed");
}

bool VSTPluginDelegate::alive() const {
    return owner_ != nullptr;
}

// owner can be nullptr (= destroyed)!
void VSTPluginDelegate::setOwner(VSTPlugin *owner) {
    if (owner) {
        // cache some members
        world_ = owner->mWorld;
    }
    owner_ = owner;
}

void VSTPluginDelegate::parameterAutomated(int index, float value) {
    // RT thread
    if (std::this_thread::get_id() == rtThreadID_) {
        // only if caused by /set! ignore UGen input automation and parameter mappings.
        if (paramSet_) {
            sendParameterAutomated(index, value);
        }
    } else {
        // from GUI thread [or NRT thread] - push to queue
        ScopedLock lock(owner_->paramQueueMutex_);
        if (!(owner_->paramQueue_.emplace(index, value))){
            LOG_DEBUG("param queue overflow");
        }
    }
}

void VSTPluginDelegate::latencyChanged(int nsamples){
    // RT thread
    if (std::this_thread::get_id() == rtThreadID_) {
        sendLatencyChange(nsamples);
    } else {
        // from GUI thread [or NRT thread] - push to queue
        ScopedLock lock(owner_->paramQueueMutex_);
        if (!(owner_->paramQueue_.emplace(LatencyChange, (float)nsamples))){
            LOG_DEBUG("param queue overflow");
        }
    }
}

void VSTPluginDelegate::pluginCrashed(){
    // RT thread
    if (std::this_thread::get_id() == rtThreadID_) {
        sendPluginCrash();
    } else {
        // from GUI thread [or NRT thread] - push to queue
        ScopedLock lock(owner_->paramQueueMutex_);
        if (!(owner_->paramQueue_.emplace(PluginCrash, 0.f))){
            LOG_DEBUG("param queue overflow");
        }
    }
}

void VSTPluginDelegate::midiEvent(const MidiEvent& midi) {
    // check if we're on the realtime thread, otherwise ignore it
    if (std::this_thread::get_id() == rtThreadID_) {
        float buf[3];
        // we don't want negative values here
        buf[0] = (unsigned char)midi.data[0];
        buf[1] = (unsigned char)midi.data[1];
        buf[2] = (unsigned char)midi.data[2];
        sendMsg("/vst_midi", 3, buf);
    }
}

void VSTPluginDelegate::sysexEvent(const SysexEvent & sysex) {
    // check if we're on the realtime thread, otherwise ignore it
    if (std::this_thread::get_id() == rtThreadID_) {
        if ((sysex.size * sizeof(float)) > MAX_OSC_PACKET_SIZE) {
            LOG_WARNING("sysex message (" << sysex.size << " bytes) too large for UDP packet - dropped!");
            return;
        }
        float* buf = (float*)alloca(sysex.size * sizeof(float));
        for (int i = 0; i < sysex.size; ++i) {
            // no need to cast to unsigned because SC's Int8Array is signed anyway
            buf[i] = sysex.data[i];
        }
        sendMsg("/vst_sysex", sysex.size, buf);
    }
}

bool VSTPluginDelegate::check(bool loud) const {
    if (!plugin_){
        if (loud){
            LOG_WARNING("VSTPlugin: no plugin loaded!");
        }
        return false;
    }
    if (suspended_){
        if (loud){
            LOG_WARNING("VSTPlugin: temporarily suspended!");
        }
        return false;
    }
    return true;
}

Lock VSTPluginDelegate::scopedLock(){
    return Lock(mutex_);
}

bool VSTPluginDelegate::tryLock() {
    return mutex_.try_lock();
}

void VSTPluginDelegate::unlock() {
    mutex_.unlock();
}

// try to close the plugin in the NRT thread with an asynchronous command
void VSTPluginDelegate::close() {
    if (plugin_) {
        LOG_DEBUG("about to close");
        // owner_ == nullptr -> close() called as result of this being the last reference,
        // otherwise we check if there is more than one reference.
        if (owner_ && !owner_->delegate_.unique()) {
            LOG_WARNING("VSTPlugin: can't close plugin while commands are still running");
            // will be closed by the command's cleanup function
            return;
        }
        auto cmdData = CmdData::create<CloseCmdData>(world());
        if (!cmdData) {
            return;
        }
        // unset listener!
        plugin_->setListener(nullptr);
        cmdData->plugin = std::move(plugin_);
        cmdData->editor = editor_;
        doCmd(cmdData, [](World *world, void* inData) {
            auto data = (CloseCmdData*)inData;
            defer([&](){
                // release plugin
                data->plugin = nullptr;
            }, data->editor);
            return false; // done
        });
        plugin_ = nullptr;
    }
}

bool cmdOpen(World *world, void* cmdData) {
    LOG_DEBUG("cmdOpen");
    // initialize GUI backend (if needed)
    auto data = (OpenCmdData *)cmdData;
    // create plugin in main thread
    auto info = queryPlugin(data->path);
    if (info) {
        try {
            // make sure to only request the plugin UI if the
            // plugin supports it and we have an event loop
            if (data->editor &&
                    !(info->editor() && UIThread::available())){
                data->editor = false;
                LOG_DEBUG("can't use plugin UI!");
            }
            if (data->editor){
                LOG_DEBUG("create plugin in UI thread");
            } else {
                LOG_DEBUG("create plugin in NRT thread");
            }
            defer([&](){
                // create plugin
                data->plugin = info->create(data->editor, data->threaded, data->mode);
                // setup plugin
                data->plugin->suspend();
                if (info->hasPrecision(ProcessPrecision::Single)) {
                    data->plugin->setupProcessing(data->sampleRate, data->blockSize,
                                                  ProcessPrecision::Single);
                } else {
                    LOG_WARNING("VSTPlugin: plugin '" << info->name <<
                                "' doesn't support single precision processing - bypassing!");
                }
                data->plugin->setNumSpeakers(data->numInputs, data->numOutputs,
                                             data->numAuxInputs, data->numAuxOutputs);
                data->plugin->resume();
            }, data->editor);
            LOG_DEBUG("done");
        } catch (const Error & e) {
            LOG_ERROR(e.what());
        }
    }
    return true;
}

// try to open the plugin in the NRT thread with an asynchronous command
void VSTPluginDelegate::open(const char *path, bool editor,
                             bool threaded, RunMode mode) {
    LOG_DEBUG("open");
    if (isLoading_) {
        LOG_WARNING("already loading!");
        sendMsg("/vst_open", 0);
        return;
    }
    close();
    if (plugin_) {
        LOG_ERROR("ERROR: couldn't close current plugin!");
        sendMsg("/vst_open", 0);
        return;
    }
#ifdef SUPERNOVA
    if (threaded){
        LOG_WARNING("WARNING: multiprocessing option ignored on Supernova!");
        threaded = false;
    }
#endif

    auto len = strlen(path) + 1;
    auto cmdData = CmdData::create<OpenCmdData>(world(), len);
    if (cmdData) {
        memcpy(cmdData->path, path, len);
        cmdData->editor = editor;
        cmdData->threaded = threaded;
        cmdData->mode = mode;
        cmdData->sampleRate = owner_->sampleRate();
        cmdData->blockSize = owner_->blockSize();
        cmdData->numInputs = owner_->numInChannels();
        cmdData->numOutputs = owner_->numOutChannels();
        cmdData->numAuxInputs = owner_->numAuxInChannels();
        cmdData->numAuxOutputs = owner_->numAuxOutChannels();

        doCmd(cmdData, cmdOpen, [](World *world, void *cmdData){
            auto data = (OpenCmdData*)cmdData;
            data->owner->doneOpen(*data); // alive() checked in doneOpen!
            return false; // done
        });
        isLoading_ = true;
        // NOTE: don't set 'editor_' already because 'editor' value might change
    } else {
        sendMsg("/vst_open", 0);
    }
}

// "/open" command succeeded/failed - called in the RT thread
void VSTPluginDelegate::doneOpen(OpenCmdData& cmd){
    LOG_DEBUG("doneOpen");
    editor_ = cmd.editor;
    threaded_ = cmd.threaded;
    isLoading_ = false;
    // move the plugin even if alive() returns false (so it will be properly released in close())
    plugin_ = std::move(cmd.plugin);
    if (!alive()) {
        LOG_WARNING("VSTPlugin: freed while opening a plugin");
        return; // !
    }
    if (plugin_){
        if (!plugin_->info().hasPrecision(ProcessPrecision::Single)) {
            Print("WARNING: '%s' doesn't support single precision processing - bypassing!\n",
                  plugin_->info().name.c_str());
        }
        LOG_DEBUG("opened " << cmd.path);
        // receive events from plugin
        plugin_->setListener(shared_from_this());
        // update data
        owner_->update();
        // success, window, initial latency
        bool haveWindow = plugin_->getWindow() != nullptr;
        int latency = plugin_->getLatencySamples() + latencySamples();
        float data[3] = { 1.f, (float)haveWindow, (float)latency };
        sendMsg("/vst_open", 3, data);
    } else {
        LOG_WARNING("VSTPlugin: couldn't open " << cmd.path);
        sendMsg("/vst_open", 0);
    }
}

void VSTPluginDelegate::showEditor(bool show) {
    if (plugin_ && plugin_->getWindow()) {
        auto cmdData = CmdData::create<PluginCmdData>(world());
        if (cmdData) {
            cmdData->i = show;
            doCmd(cmdData, [](World * inWorld, void* inData) {
                auto data = (PluginCmdData*)inData;
                auto window = data->owner->plugin()->getWindow();
                if (data->i) {
                    window->open();
                }
                else {
                    window->close();
                }
                return false; // done
            });
        }
    }
}

void VSTPluginDelegate::reset(bool async) {
    if (check()) {
    #if 1
        // force async if we have a plugin UI to avoid
        // race conditions with concurrent UI updates.
        if (editor_){
            async = true;
        }
    #endif
        if (async) {
            // reset in the NRT thread
            suspended_ = true; // suspend
            doCmd(CmdData::create<PluginCmdData>(world()),
                [](World *world, void *cmdData){
                    auto data = (PluginCmdData *)cmdData;
                    defer([&](){
                        auto lock = data->owner->scopedLock();
                        data->owner->plugin()->suspend();
                        data->owner->plugin()->resume();
                    }, data->owner->hasEditor());
                    return true; // continue
                },
                [](World *world, void *cmdData){
                    auto data = (PluginCmdData *)cmdData;
                    data->owner->suspended_ = false; // resume
                    return false; // done
                }
            );
        } else {
            // reset in the RT thread
            auto lock = scopedLock(); // avoid concurrent read/writes
            plugin_->suspend();
            plugin_->resume();
        }
    }
}

void VSTPluginDelegate::setParam(int32 index, float value) {
    if (check()){
        if (index >= 0 && index < plugin_->info().numParameters()) {
#ifdef SUPERNOVA
            // set the RT thread back to the main audio thread (see "next")
            rtThreadID_ = std::this_thread::get_id();
#endif
            paramSet_ = true;
            plugin_->setParameter(index, value, owner_->mWorld->mSampleOffset);
            float newValue = plugin_->getParameter(index);
            owner_->paramState_[index] = newValue;
            sendParameter(index, newValue);
            owner_->unmap(index);
            paramSet_ = false;
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::setParam(int32 index, const char* display) {
    if (check()){
        if (index >= 0 && index < plugin_->info().numParameters()) {
#ifdef SUPERNOVA
            // set the RT thread back to the main audio thread (see "next")
            rtThreadID_ = std::this_thread::get_id();
#endif
            paramSet_ = true;
            if (!plugin_->setParameter(index, display, owner_->mWorld->mSampleOffset)) {
                LOG_WARNING("VSTPlugin: couldn't set parameter " << index << " to " << display);
            }
            float newValue = plugin_->getParameter(index);
            owner_->paramState_[index] = newValue;
            sendParameter(index, newValue);
            owner_->unmap(index);
            paramSet_ = false;
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::queryParams(int32 index, int32 count) {
    if (check(false)) {
        int32 nparam = plugin_->info().numParameters();
        if (index >= 0 && index < nparam) {
            count = std::min<int32>(count, nparam - index);
            for (int i = 0; i < count; ++i) {
                sendParameter(index + i, plugin_->getParameter(index + i));
            }
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::getParam(int32 index) {
    float msg[2] = { (float)index, 0.f };

    if (check()) {
        if (index >= 0 && index < plugin_->info().numParameters()) {
            msg[1] = plugin_->getParameter(index); // value
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }

    sendMsg("/vst_set", 2, msg);
}

void VSTPluginDelegate::getParams(int32 index, int32 count) {
    if (check()) {
        int32 nparam = plugin_->info().numParameters();
        if (index >= 0 && index < nparam) {
            if (count < 0){
                count = nparam - index;
            } else {
                count = std::min<int32>(count, nparam - index);
            }
            const int bufsize = count + 2; // for index + count
            if (bufsize * sizeof(float) < MAX_OSC_PACKET_SIZE){
                float *buf = (float *)alloca(sizeof(float) * bufsize);
                buf[0] = index;
                buf[1] = count;
                for (int i = 0; i < count; ++i) {
                    float value = plugin_->getParameter(i + index);
                    buf[i + 2] = value;
                }
                sendMsg("/vst_setn", bufsize, buf);
                return;
            } else {
                LOG_WARNING("VSTPlugin: too many parameters requested!");
            }
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
    // send empty reply (count = 0)
    float msg[2] = { (float)index, 0.f };
    sendMsg("/vst_setn", 2, msg);
}

void VSTPluginDelegate::mapParam(int32 index, int32 bus, bool audio) {
    if (check()) {
        if (index >= 0 && index < plugin_->info().numParameters()) {
            owner_->map(index, bus, audio);
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::unmapParam(int32 index) {
    if (check()) {
        if (index >= 0 && index < plugin_->info().numParameters()) {
            owner_->unmap(index);
        }
        else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::unmapAll() {
    if (check()) {
        owner_->clearMapping();
    }
}

// program/bank
void VSTPluginDelegate::setProgram(int32 index) {
    if (check()) {
        if (index >= 0 && index < plugin_->info().numPrograms()) {
            plugin_->setProgram(index);
        }
        else {
            LOG_WARNING("VSTPlugin: program number " << index << " out of range!");
        }
    }
    sendMsg("/vst_program_index", plugin_->getProgram());
}

void VSTPluginDelegate::setProgramName(const char *name) {
    if (check()) {
        plugin_->setProgramName(name);
        sendCurrentProgramName();
    }
}

void VSTPluginDelegate::queryPrograms(int32 index, int32 count) {
    if (check(false)) {
        int32 nprogram = plugin_->info().numPrograms();
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
    auto data = (PresetCmdData*)cmdData;
    auto plugin = data->owner->plugin();
    auto& buffer = data->buffer;
    bool async = data->async;
    bool result = true;
    try {
        if (data->bufnum < 0) {
            // from file
            vst::File file(data->path);
            if (!file.is_open()){
                throw Error("couldn't open file " + std::string(data->path));
            }
            file.seekg(0, std::ios_base::end);
            buffer.resize(file.tellg());
            file.seekg(0, std::ios_base::beg);
            file.read(&buffer[0], buffer.size());
        }
        else {
            // from buffer
            auto sndbuf = World_GetNRTBuf(world, data->bufnum);
            writeBuffer(sndbuf, buffer);
        }
        if (async) {
            // load preset now
            // NOTE: we avoid readProgram() to minimize the critical section
            defer([&](){
                auto lock = data->owner->scopedLock();
                if (bank)
                    plugin->readBankData(buffer);
                else
                    plugin->readProgramData(buffer);
            }, data->owner->hasEditor());
        }
    } catch (const Error& e) {
        Print("ERROR: couldn't read %s: %s\n", (bank ? "bank" : "program"), e.what());
        result = false;
    }
    data->result = result;
    return true;
}

template<bool bank>
bool cmdReadPresetDone(World *world, void *cmdData){
    auto data = (PresetCmdData *)cmdData;
    if (!data->alive()) return false;
    auto owner = data->owner;

    if (data->async){
        data->owner->resume();
    } else if (data->result) {
        // read preset data
        try {
            auto lock = data->owner->scopedLock(); // avoid concurrent read/write
            if (bank)
                owner->plugin()->readBankData(data->buffer);
            else
                owner->plugin()->readProgramData(data->buffer);
        } catch (const Error& e){
            Print("ERROR: couldn't read %s: %s\n", (bank ? "bank" : "program"), e.what());
            data->result = 0;
        }
    }

    if (bank) {
        owner->sendMsg("/vst_bank_read", data->result);
        // a bank change also sets the current program number!
        owner->sendMsg("/vst_program_index", owner->plugin()->getProgram());
    }
    else {
        owner->sendMsg("/vst_program_read", data->result);
    }
    // the program name has most likely changed
    owner->sendCurrentProgramName();

    return true; // continue
}

template<bool bank, typename T>
void VSTPluginDelegate::readPreset(T dest, bool async){
    if (check()){
    #if 1
        // force async if we have a plugin UI to avoid
        // race conditions with concurrent UI updates.
        if (editor_){
            async = true;
        }
    #endif
        if (async){
            suspended_ = true;
        }
        doCmd(PresetCmdData::create(world(), dest, async),
            cmdReadPreset<bank>, cmdReadPresetDone<bank>, PresetCmdData::nrtFree);
    } else {
        if (bank) {
            sendMsg("/vst_bank_read", 0);
        }
        else {
            sendMsg("/vst_program_read", 0);
        }
    }
}

template<bool bank>
bool cmdWritePreset(World *world, void *cmdData){
    auto data = (PresetCmdData *)cmdData;
    auto plugin = data->owner->plugin();
    auto& buffer = data->buffer;
    bool async = data->async;
    bool result = true;
    try {
        // NOTE: we avoid writeProgram() to minimize the critical section
        if (async){
            // try to move memory allocation *before* the lock,
            // so we keep the critical section as short as possible.
            buffer.reserve(1024);
            // get and write preset data
            defer([&](){
                auto lock = data->owner->scopedLock();
                if (bank)
                    plugin->writeBankData(buffer);
                else
                    plugin->writeProgramData(buffer);
            }, data->owner->hasEditor());
        }
        if (data->bufnum < 0) {
            // write data to file
            vst::File file(data->path, File::WRITE);
            if (!file.is_open()){
                throw Error("couldn't create file " + std::string(data->path));
            }
            file.write(buffer.data(), buffer.size());
        }
        else {
            // to buffer
            auto sndbuf = World_GetNRTBuf(world, data->bufnum);
            data->freeData = sndbuf->data; // to be freed in stage 4
            allocReadBuffer(sndbuf, buffer);
        }
    } catch (const Error & e) {
        Print("ERROR: couldn't write %s: %s\n", (bank ? "bank" : "program"), e.what());
        result = false;
    }
    data->result = result;
    return true;
}

template<bool bank>
bool cmdWritePresetDone(World *world, void *cmdData){
    auto data = (PresetCmdData *)cmdData;
    if (!data->alive()) return true; // will just free data
    if (data->async){
        data->owner->resume();
    }
    if (data->bufnum >= 0){
        syncBuffer(world, data->bufnum);
    }
    data->owner->sendMsg(bank ? "/vst_bank_write" : "/vst_program_write", data->result);
    return true; // continue
}

template<bool bank, typename T>
void VSTPluginDelegate::writePreset(T dest, bool async) {
    if (check()) {
        auto data = PresetCmdData::create(world(), dest, async);
    #if 1
        // force async if we have a plugin UI to avoid
        // race conditions with concurrent UI updates.
        if (editor_){
            async = true;
        }
    #endif
        if (async){
            suspended_ = true;
        } else {
            try {
                auto lock = scopedLock(); // avoid concurrent read/write
                if (bank){
                    plugin_->writeBankData(data->buffer);
                } else {
                    plugin_->writeProgramData(data->buffer);
                }
            } catch (const Error& e){
                Print("ERROR: couldn't write %s: %s\n", (bank ? "bank" : "program"), e.what());
                goto fail;
            }
        }
        doCmd(data, cmdWritePreset<bank>, cmdWritePresetDone<bank>, PresetCmdData::nrtFree);
    } else {
    fail:
        if (bank) {
            sendMsg("/vst_bank_write", 0);
        }
        else {
            sendMsg("/vst_program_write", 0);
        }
    }
}

// midi
void VSTPluginDelegate::sendMidiMsg(int32 status, int32 data1, int32 data2, float detune) {
    if (check()) {
        plugin_->sendMidiEvent(MidiEvent(status, data1, data2, world_->mSampleOffset, detune));
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
    } else {
        sendMsg("/vst_transport", -1);
    }
}

// advanced

void VSTPluginDelegate::canDo(const char *what) {
    if (check()) {
        auto result = plugin_->canDo(what);
        sendMsg("/vst_can_do", (float)result);
    } else {
        sendMsg("/vst_can_do", 0);
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
        }
        else {
            auto result = plugin_->vendorSpecific(index, value, (void *)data, opt);
            sendMsg("/vst_vendor_method", (float)result);
        }
    } else {
        sendMsg("/vst_vendor_method", 0);
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
    if (plugin_->info().numPrograms() > 0){
        const int maxSize = 64;
        float buf[maxSize];
        // msg format: index, len, characters...
        buf[0] = plugin_->getProgram();
        int size = string2floatArray(plugin_->getProgramName(), buf + 1, maxSize - 1);
        sendMsg("/vst_program", size + 1, buf);
    }
}

// unchecked
void VSTPluginDelegate::sendParameter(int32 index, float value) {
    const int maxSize = 64;
    float buf[maxSize];
    // msg format: index, value, display length, display chars...
    buf[0] = index;
    buf[1] = value;
    int size = string2floatArray(plugin_->getParameterString(index), buf + 2, maxSize - 2);
    sendMsg("/vst_param", size + 2, buf);
}

// unchecked
void VSTPluginDelegate::sendParameterAutomated(int32 index, float value) {
    sendParameter(index, value);
    float buf[2] = { (float)index, value };
    sendMsg("/vst_auto", 2, buf);
}

int32 VSTPluginDelegate::latencySamples() const {
    int32 blockSize = owner_->blockSize();
    int32 nsamples = blockSize - world_->mBufLength;
    if (threaded_){
        nsamples += blockSize;
    }
    return nsamples;
}

void VSTPluginDelegate::sendLatencyChange(int nsamples){
    sendMsg("/vst_latency", nsamples + latencySamples());
}

void VSTPluginDelegate::sendPluginCrash(){
    sendMsg("/vst_crash", 0);
}

void VSTPluginDelegate::sendMsg(const char *cmd, float f) {
    if (owner_){
        SendNodeReply(&owner_->mParent->mNode, owner_->mParentIndex, cmd, 1, &f);
    }
    else {
        LOG_ERROR("BUG: VSTPluginDelegate::sendMsg");
    }
}

void VSTPluginDelegate::sendMsg(const char *cmd, int n, const float *data) {
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
template<typename T>
void cmdRTfree(World *world, void * cmdData) {
    if (cmdData) {
        auto data = (T*)cmdData;
        data->~T(); // destruct members (e.g. release rt::shared_pointer in RT thread)
        RTFree(world, cmdData);
        LOG_DEBUG("cmdRTfree!");
    }
}

template<typename T>
void VSTPluginDelegate::doCmd(T *cmdData, AsyncStageFn stage2,
    AsyncStageFn stage3, AsyncStageFn stage4) {
    // so we don't have to always check the return value of makeCmdData
    if (cmdData) {
        // Don't store pointer to this if we're about to get destructed!
        // This is mainly for the 'close' command which might get sent
        // in ~VSTPluginDelegate (and doesn't need the owner).
        if (alive()) {
            cmdData->owner = shared_from_this();
        }
        DoAsynchronousCommand(world(),
            0, 0, cmdData, stage2, stage3, stage4, cmdRTfree<T>, 0, 0);
    }
}

/*** unit command callbacks ***/

void vst_open(VSTPlugin *unit, sc_msg_iter *args) {
    const char *path = args->gets();
    auto editor = args->geti();
    auto threaded = args->geti();

    RunMode mode;
    switch (args->geti()){
    case 1:
        mode = RunMode::Sandbox;
        break;
    case 2:
        mode = RunMode::Bridge;
        break;
    default:
        mode = RunMode::Auto;
        break;
    }

    if (path) {
        unit->delegate().open(path, editor, threaded, mode);
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

// helper function
bool vst_param_index(VSTPlugin* unit, sc_msg_iter *args, int& index) {
    if (args->nextTag() == 's') {
        auto name = args->gets();
        auto plugin = unit->delegate().plugin();
        if (plugin){
            index = plugin->info().findParam(name);
            if (index < 0) {
                LOG_ERROR("parameter '" << name << "' not found!");
                return false;
            }
        } else {
            LOG_WARNING("no plugin loaded!");
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

void vst_domap(VSTPlugin* unit, sc_msg_iter* args, bool audio) {
    if (unit->delegate().check()) {
        while (args->remain() > 0) {
            int32 index = -1;
            if (vst_param_index(unit, args, index)) {
                int32 bus = args->geti(-1);
                int32 numChannels = args->geti();
                for (int i = 0; i < numChannels; ++i) {
                    unit->delegate().mapParam(index + i, bus + i, audio);
                }
            }
            else {
                args->geti(); // swallow bus
                args->geti(); // swallow numChannels
            }
        }
    }
}

// map parameters to control busses
void vst_map(VSTPlugin* unit, sc_msg_iter *args) {
    vst_domap(unit, args, false);
}

// map parameters to audio busses
void vst_mapa(VSTPlugin* unit, sc_msg_iter* args) {
    vst_domap(unit, args, true);
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
            unit->delegate().unmapAll();
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
        const char* name = args->gets(); // file name
        bool async = args->geti();
        unit->delegate().readPreset<false>(name, async);
    }
    else {
        int32 buf = args->geti(); // buf num
        bool async = args->geti();
        if (buf >= 0 && buf < (int)unit->mWorld->mNumSndBufs) {
            unit->delegate().readPreset<false>(buf, async);
        } else {
            LOG_ERROR("vst_program_read: bufnum " << buf << " out of range");
        }
    }
}

void vst_program_write(VSTPlugin *unit, sc_msg_iter *args) {
    if (args->nextTag() == 's') {
        const char* name = args->gets(); // file name
        bool async = args->geti();
        unit->delegate().writePreset<false>(name, async);
    } else {
        int32 buf = args->geti(); // buf num
        bool async = args->geti();
        if (buf >= 0 && buf < (int)unit->mWorld->mNumSndBufs) {
            unit->delegate().writePreset<false>(buf, async);
        } else {
            LOG_ERROR("vst_program_write: bufnum " << buf << " out of range");
        }
    }
}

void vst_bank_read(VSTPlugin* unit, sc_msg_iter *args) {
    if (args->nextTag() == 's') {
        const char* name = args->gets(); // file name
        bool async = args->geti();
        unit->delegate().readPreset<true>(name, async);
    }
    else {
        int32 buf = args->geti(); // buf num
        bool async = args->geti();
        if (buf >= 0 && buf < (int)unit->mWorld->mNumSndBufs) {
            unit->delegate().readPreset<true>(buf, async);
        } else {
            LOG_ERROR("vst_bank_read: bufnum " << buf << " out of range");
        }
    }
}

void vst_bank_write(VSTPlugin* unit, sc_msg_iter *args) {
    if (args->nextTag() == 's') {
        const char* name = args->gets(); // file name
        bool async = args->geti();
        unit->delegate().writePreset<true>(name, async);
    } else {
        int32 buf = args->geti(); // buf num
        bool async = args->geti();
        if (buf >= 0 && buf < (int)unit->mWorld->mNumSndBufs) {
            unit->delegate().writePreset<true>(buf, async);
        } else {
            LOG_ERROR("vst_bank_write: bufnum " << buf << " out of range");
        }
    }
}

void vst_midi_msg(VSTPlugin* unit, sc_msg_iter *args) {
    char data[4];
    int32 len = args->getbsize();
    if (len > 4) {
        LOG_WARNING("vst_midi_msg: midi message too long (" << len << " bytes)");
    }
    args->getb(data, len);
    auto detune = args->getf();
    unit->delegate().sendMidiMsg(data[0], data[1], data[2], detune);
}

void vst_midi_sysex(VSTPlugin* unit, sc_msg_iter *args) {
    int len = args->getbsize();
    if (len < 0){
        LOG_WARNING("vst_midi_sysex: no data!");
        return;
    }
    if (len > 65536){
        // arbitrary limit (can only be reached with TCP)
        LOG_WARNING("vst_midi_sysex: message exceeding internal limit of 64 kB");
        return;
    }
    // LATER avoid unnecessary copying.
    char *buf = (char *)alloca(len);
    args->getb(buf, len);
    unit->delegate().sendSysexMsg(buf, len);
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
        if (size > 65536){
            // arbitrary limit (can only be reached with TCP)
            LOG_WARNING("vst_vendor_method: message exceeding internal limit of 64 kB");
            return;
        }
        data = (char *)alloca(size);
        args->getb(data, size);
    }
    float opt = args->getf();
    bool async = args->geti();
    unit->delegate().vendorSpecific(index, value, size, data, opt, async);
}

/*** plugin command callbacks ***/

// recursively search directories for VST plugins.
bool cmdSearch(World *inWorld, void* cmdData) {
    auto data = (SearchCmdData *)cmdData;
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
        if (gSearching){
            auto result = searchPlugins(path, parallel, verbose);
            plugins.insert(plugins.end(), result.begin(), result.end());
        } else {
            save = false; // don't update cache file
            LOG_DEBUG("search cancelled");
            break;
        }
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
    auto data = (SearchCmdData*)cmdData;
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
    } else {
        bufnum = args->geti();
        // negative bufnum allowed (= don't write result)!
        if (bufnum >= (int)inWorld->mNumSndBufs) {
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

    SearchCmdData *data = CmdData::create<SearchCmdData>(inWorld, pathLen);
    if (data) {
        data->flags = flags;
        data->bufnum = bufnum; // negative bufnum: don't write search result
        if (filename) {
            snprintf(data->path, sizeof(data->path), "%s", filename);
        } else {
            data->path[0] = '\0'; // empty path: use buffer
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
        gSearching = true; // before command dispatching! -> NRT mode
        DoAsynchronousCommand(inWorld, replyAddr, "vst_search", data,
            cmdSearch, cmdSearchDone, SearchCmdData::nrtFree, cmdRTfree, 0, 0);
    }
}

void vst_search_stop(World* inWorld, void* inUserData, struct sc_msg_iter*args, void* replyAddr) {
    gSearching = false;
}

void vst_clear(World* inWorld, void* inUserData, struct sc_msg_iter* args, void* replyAddr) {
    if (!gSearching) {
        auto data = CmdData::create<PluginCmdData>(inWorld);
        if (data) {
            data->i = args->geti(); // 1 = remove cache file
            DoAsynchronousCommand(inWorld, replyAddr, "vst_clear", data, [](World*, void* data) {
                // unloading plugins might crash, so we make sure we *first* delete the cache file
                int flags = static_cast<PluginCmdData*>(data)->i;
                if (flags & 1) {
                    // remove cache file
                    removeFile(getSettingsDir() + "/" CACHE_FILE);
                }
                gPluginManager.clear();
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
    auto data = (SearchCmdData *)cmdData;
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
    auto data = (SearchCmdData*)cmdData;
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
    int32 bufnum = -1;
    const char* filename = nullptr;
    auto path = args->gets(); // plugin path
    auto size = strlen(path) + 1;
    // temp file or buffer to store the probe result
    if (args->nextTag() == 's') {
        filename = args->gets();
    } else {
        bufnum = args->geti();
        // negative bufnum allowed (= don't write result)!
        if (bufnum >= (int)inWorld->mNumSndBufs) {
            LOG_ERROR("vst_probe: bufnum " << bufnum << " out of range");
            return;
        }
    }

    auto data = CmdData::create<SearchCmdData>(inWorld, size);
    if (data) {
        data->bufnum = bufnum;
        // temp file or buffer to store the plugin info
        if (filename) {
            snprintf(data->path, sizeof(data->path), "%s", filename);
        } else {
            data->path[0] = '\0'; // empty path: use buffer
        }

        memcpy(data->buf, path, size); // store plugin path

        DoAsynchronousCommand(inWorld, replyAddr, "vst_probe",
            data, cmdProbe, cmdProbeDone, SearchCmdData::nrtFree, cmdRTfree, 0, 0);
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
// This means we can't check for 0 or nullptrs... The current solution is to (ab)use 'specialIndex',
// which *is* set to zero.
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
    UnitCmd(mapa);
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
    PluginCmd(vst_search_stop);
    PluginCmd(vst_clear);
    PluginCmd(vst_probe);

    setLogFunction(SCLog);

    Print("VSTPlugin %s\n", getVersionString());
    // read cached plugin info
    readIniFile();
}


