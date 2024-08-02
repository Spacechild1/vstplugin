#include "VSTPlugin.h"

#ifdef SUPERNOVA
#include <nova-tt/spin_lock.hpp>
#include <nova-tt/rw_spinlock.hpp>
#endif

static InterfaceTable *ft;

// TODO: Multiple Server instances would mutually override the verbosity...
// In practice, this is not a big issue because people mostly use a single Server per process.
static std::atomic<int> gVerbosity{0};

void setVerbosity(int verbosity){
    gVerbosity.store(verbosity);
}

int getVerbosity(){
    return gVerbosity.load(std::memory_order_relaxed);
}

void SCLog(int level, const char *s){
    // verbosity 0: print everything
    // verbosity -1: only errors
    // verbosity -2: nothing
    auto verbosity = getVerbosity();
    if (verbosity >= 0 || (verbosity == -1 && (level == 0))) {
        if (level == 0){
            Print("ERROR: %s", s);
        } else if (level == 1) {
            Print("WARNING: %s", s);
        } else {
            Print("%s", s);
        }
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

static void allocReadBuffer(SndBuf* buf, std::string_view data) {
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
    } else {
        LOG_ERROR("RTAlloc failed!");
        return nullptr;
    }
}

// Check if the Unit is still alive. Should only be called in RT stages!
bool CmdData::alive() const {
    auto b = owner->alive();
    if (!b) {
        LOG_WARNING("VSTPlugin freed during background task");
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
int string2floatArray(std::string_view src, float *dest, int maxSize) {
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
        Error err;
        bool ok = UIThread::callSync([&](){
            try {
                fn();
            } catch (const Error& e){
                err = e;
            }
        });
        if (ok){
            if (err.code() != Error::NoError){
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

static PluginDictionary gPluginDict;

static std::string gSettingsDir = userSettingsPath() + "/sc";

static std::string gCacheFileName = std::string("cache_")
        + cpuArchToString(getHostCpuArchitecture()) + ".ini";

static Mutex gFileLock;

static void readCacheFile(const std::string& dir, bool loud) {
    std::lock_guard lock(gFileLock);
    auto path = dir + "/" + gCacheFileName;
    if (pathExists(path)){
        LOG_VERBOSE("read cache file " << path);
        try {
            gPluginDict.read(path);
        } catch (const Error& e){
            LOG_ERROR("couldn't read cache file: " << e.what());
        } catch (const std::exception& e){
            LOG_ERROR("couldn't read cache file: unexpected exception (" << e.what() << ")");
        }
    } else if (loud) {
        LOG_ERROR("could not find cache file in " << dir);
    }
}

static void readCacheFile(){
    readCacheFile(gSettingsDir, false);
}

static void writeCacheFile(const std::string& dir) {
    std::lock_guard lock(gFileLock);
    try {
        if (pathExists(dir)) {
            gPluginDict.write(dir + "/" + gCacheFileName);
        } else {
            throw Error("directory " + dir + " does not exist");
        }
    } catch (const Error& e) {
        LOG_ERROR("couldn't write cache file: " << e.what());
    }
}

static void writeCacheFile() {
    std::lock_guard lock(gFileLock);
    try {
        if (!pathExists(gSettingsDir)) {
            createDirectory(userSettingsPath());
            if (!createDirectory(gSettingsDir)) {
                throw Error("couldn't create directory " + gSettingsDir);
            }
        }
        gPluginDict.write(gSettingsDir + "/" + gCacheFileName);
    } catch (const Error& e) {
        LOG_ERROR("couldn't write cache file: " << e.what());
    }
}

static PluginDictionary& getPluginDict() {
    static bool once = []() {
        readCacheFile();
        return true;
    }();
    return gPluginDict;
}

void serializePlugin(std::ostream& os, const PluginDesc& desc) {
    desc.serialize(os);
    os << "[keys]\n";
    os << "n=1\n";
    os << desc.key() << "\n";
}

// load factory and probe plugins

static IFactory::ptr loadFactory(const std::string& path, bool verbose = false){
    IFactory::ptr factory;
    auto& dict = getPluginDict();

    if (dict.findFactory(path)) {
        LOG_ERROR("bug in 'loadFactory'");
        return nullptr;
    }
    if (dict.isException(path)) {
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
        LOG_ERROR("couldn't load '" << path << "': " << e.what());
        dict.addException(path);
        return nullptr;
    }

    return factory;
}

static void addFactory(const std::string& path, IFactory::ptr factory){
    auto& dict = getPluginDict();
    if (factory->numPlugins() == 1) {
        auto plugin = factory->getPlugin(0);
        // factories with a single plugin can also be aliased by their file path(s)
        dict.addPlugin(plugin->path(), plugin);
        dict.addPlugin(path, plugin);
    }
    dict.addFactory(path, factory);
    for (int i = 0; i < factory->numPlugins(); ++i) {
        auto plugin = factory->getPlugin(i);
    #if 0
        // search for presets
        const_cast<PluginDesc&>(*plugin).scanPresets();
    #endif
        dict.addPlugin(plugin->key(), plugin);
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

static IFactory::ptr probePlugin(const std::string& path,
                                 float timeout, bool verbose) {
    auto factory = loadFactory(path, verbose);
    if (!factory){
        return nullptr;
    }

    if (verbose) {
        Print("probing %s... ", path.c_str());
    }

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
        }, timeout);
        if (factory->valid()){
            addFactory(path, factory);
            return factory; // success
        }
    } catch (const Error& e){
        if (verbose) postResult(e);
    }
    getPluginDict().addException(path);
    return nullptr;
}

using FactoryFutureResult = std::pair<bool, IFactory::ptr>;
using FactoryFuture = std::function<FactoryFutureResult()>;

static FactoryFuture probePluginAsync(const std::string& path,
                                      float timeout, bool verbose) {
    auto factory = loadFactory(path, verbose);
    if (!factory){
        return []() -> FactoryFutureResult {
            return { true, nullptr };
        };
    }
    // start probing process
    try {
        auto future = factory->probeAsync(timeout, true);
        // return future
        return [=]() -> FactoryFutureResult {
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
                    return { true, factory }; // success
                } else {
                    getPluginDict().addException(path);
                    return { true, nullptr };
                }
            } else {
                return { false, nullptr }; // not ready
            }
        };
    } catch (const Error& e) {
        // return future which prints the error message
        return [=]() -> FactoryFutureResult {
            if (verbose) {
                Print("probing %s... ", path.c_str());
                postResult(e);
            }
            getPluginDict().addException(path);
            return { true, nullptr };
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

// resolves relative path to an existing plugin in the VST search paths.
// returns empty string on failure!
static std::string resolvePluginPath(const std::string& s) {
    auto path = normalizePath(s);
    if (isAbsolutePath(path)) {
        return path; // success
    }
    if (fileExtension(path).empty()){
        // no extension: assume VST2 plugin
    #ifdef _WIN32
        path += ".dll";
    #elif defined(__APPLE__)
        path += ".vst";
    #else // Linux/BSD/etc.
        path += ".so";
    #endif
    }
    for (auto& dir : getDefaultSearchPaths()) {
        auto result = vst::find(dir, path);
        if (!result.empty()) return result; // success
    }
    return std::string{}; // fail
}

// query a plugin by its key or file path and probe if necessary.
static const PluginDesc* queryPlugin(const std::string& path) {
    // first try as key
    auto desc = getPluginDict().findPlugin(path);
    if (!desc) {
        // then try as file path
        auto absPath = resolvePluginPath(path);
        if (!absPath.empty()){
            desc = getPluginDict().findPlugin(absPath);
            if (!desc) {
                // finally probe plugin
                if (probePlugin(absPath, 0, getVerbosity() >= 0)) {
                    desc = getPluginDict().findPlugin(absPath);
                    // findPlugin() fails if the module contains several plugins,
                    // which means the path can't be used as a key.
                    if (!desc){
                        LOG_WARNING("'" << absPath << "' contains more than one plugin.\n"
                                    "Please perform a search and open the desired plugin by its name.");
                    }
                }
            }
        } else {
            LOG_WARNING("'" << path << "' is neither an existing plugin name nor a valid file path.");
        }
    }
    return desc.get();
}

#define PROBE_FUTURES 8

#if WARN_VST3_PARAMETERS
// HACK
static thread_local std::vector<PluginDesc::const_ptr> gWarnPlugins;
#endif

std::vector<PluginDesc::const_ptr> searchPlugins(const std::string& path,
                                                 const std::vector<std::string>& exclude,
                                                 float timeout, bool parallel, bool verbose) {
    LOG_VERBOSE("searching in '" << path << "'...");

    std::vector<PluginDesc::const_ptr> results;

    auto addPlugin = [&](PluginDesc::const_ptr plugin, int which = 0, int n = 0){
        if (verbose && n > 0) {
            Print("\t[%d/%d] %s\n", which + 1, n, plugin->name.c_str());
        }
        results.push_back(plugin);
    };

    std::vector<std::pair<FactoryFuture, std::string>> futures;

    auto last = std::chrono::system_clock::now();

    auto processFutures = [&](int limit){
        while (futures.size() > limit){
            bool didSomething = false;
            for (auto it = futures.begin(); it != futures.end(); ){
                if (auto [done, factory] = it->first(); done) {
                    // future finished
                    if (factory){
                        for (int i = 0; i < factory->numPlugins(); ++i){
                            auto plugin = factory->getPlugin(i);
                            addPlugin(plugin);
                        #if WARN_VST3_PARAMETERS
                            if (plugin->warnParameters) {
                                gWarnPlugins.push_back(plugin);
                            }
                        #endif
                        }
                    }
                    // remove future
                    it = futures.erase(it);
                    didSomething = true;
                } else {
                    it++;
                }
            }
            auto now = std::chrono::system_clock::now();
            if (didSomething){
                last = now;
            } else {
                using seconds = std::chrono::duration<double>;
                auto elapsed = std::chrono::duration_cast<seconds>(now - last).count();
                if (elapsed > 4.0){
                    for (auto& x : futures){
                        LOG_VERBOSE("waiting for '" << x.second << "'...");
                    }
                    last = now;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    auto& dict = getPluginDict();

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
        if (auto factory = dict.findFactory(pluginPath)) {
            // just post names of valid plugins
            if (verbose) {
                LOG_VERBOSE(pluginPath);
            }

            auto numPlugins = factory->numPlugins();
            // add and post plugins
            if (numPlugins == 1) {
                addPlugin(factory->getPlugin(0));
            } else {
                for (int i = 0; i < numPlugins; ++i) {
                    addPlugin(factory->getPlugin(i), i, numPlugins);
                }
            }
            // make sure we have the plugin keys!
            for (int i = 0; i < numPlugins; ++i){
                auto plugin = factory->getPlugin(i);
                dict.addPlugin(plugin->key(), plugin);
            }
        } else {
            // probe (will post results and add plugins)
            if (parallel){
                futures.emplace_back(probePluginAsync(pluginPath, timeout, verbose), pluginPath);
                processFutures(PROBE_FUTURES);
            } else {
                if (auto factory = probePlugin(pluginPath, timeout, verbose)) {
                    int numPlugins = factory->numPlugins();
                    for (int i = 0; i < numPlugins; ++i) {
                        auto plugin = factory->getPlugin(i);
                        addPlugin(plugin);
                    #if WARN_VST3_PARAMETERS
                        if (plugin->warnParameters) {
                            gWarnPlugins.push_back(plugin);
                        }
                    #endif
                    }
                }
            }
        }
    }, true, exclude);

    processFutures(0);

    int numResults = results.size();
    if (numResults == 1){
        LOG_VERBOSE("found 1 plugin");
    } else {
        LOG_VERBOSE("found " << numResults << " plugins");
    }
    return results;
}

// -------------------- VSTPlugin ------------------------ //

namespace {

thread_local bool gCurrentThreadRT;

// some callbacks need to know whether they are called from a RT thread,
// e.g. so they would use the appropriate memory management functions.
// this is simpler and faster than saving and checking thread IDs.
void setCurrentThreadRT() {
    gCurrentThreadRT = true;
}

bool isCurrentThreadRT() {
    // LOG_DEBUG("isCurrentThreadRT: " << gCurrentThreadRT);
    return gCurrentThreadRT;
}

} // namespace

VSTPlugin::VSTPlugin(){
    setVerbosity(mWorld->mVerbosity);
    // the following will mark this thread as a RT thread; this is used in the
    // IPluginInterface callbacks, e.g. VSTPluginDelegate::parameterAutomated().
    // NOTE: in Supernova the constructor might actually run on a DSP helper thread,
    // so we also have to do this in runUnitCmd()!
    setCurrentThreadRT(); // !
    // Ugen inputs:
    //   flags, blocksize, bypass, ninputs, inputs..., noutputs, outputs..., nparams, params...
    //     input: nchannels, chn1, chn2, ...
    //     output: nchannels
    //     params: index, value
    assert(numInputs() >= 6);
    // int flags = in0(0);
    int reblockSize = in0(1);
    // int bypass = in0(2);

    int offset = 3;

    // setup input busses
    {
        int nin = in0(offset);
        assert(nin >= 0);
        offset++;
        // at least 1 (empty) bus for simplicity
        ugenInputs_ = (Bus *)RTAlloc(mWorld, std::max<int>(1, nin) * sizeof(Bus));
        if (ugenInputs_){
            if (nin > 0){
                LOG_DEBUG("inputs:");
                for (int i = 0; i < nin; ++i){
                    assert(offset < numInputs());
                    int nchannels = in0(offset);
                    offset++;
                    ugenInputs_[i].numChannels = nchannels;
                    ugenInputs_[i].channelData = mInBuf + offset;
                    offset += nchannels;
                    assert(offset <= numInputs());
                    LOG_DEBUG("  bus " << i << ": " << nchannels << "ch");
                }
                numUgenInputs_ = nin;
            } else {
                LOG_DEBUG("inputs: none");
                ugenInputs_[0].channelData = nullptr;
                ugenInputs_[0].numChannels = 0;
                numUgenInputs_ = 1;
            }
        } else {
            numUgenInputs_ = 0;
        }
    }
    // setup output busses
    {
        int nout = in0(offset);
        assert(nout >= 0);
        offset++;
        auto out = mOutBuf;
        auto end = mOutBuf + numOutputs();
        // at least 1 (empty) bus for simplicity
        ugenOutputs_ = (Bus *)RTAlloc(mWorld, std::max<int>(1, nout) * sizeof(Bus));
        if (ugenOutputs_){
            if (nout > 0){
                LOG_DEBUG("outputs:");
                for (int i = 0; i < nout; ++i){
                    assert(offset < numInputs());
                    int nchannels = in0(offset);
                    offset++;
                    ugenOutputs_[i].numChannels = nchannels;
                    ugenOutputs_[i].channelData = out;
                    out += nchannels;
                    assert(out <= end);
                    LOG_DEBUG("  bus " << i << ": " << nchannels << "ch");
                }
                numUgenOutputs_ = nout;
            } else {
                LOG_DEBUG("outputs: none");
                ugenOutputs_[0].channelData = nullptr;
                ugenOutputs_[0].numChannels = 0;
                numUgenOutputs_ = 1;
            }
        } else {
            numUgenOutputs_ = 0;
        }
    }

    // parameter controls
    {
        int nparams = in0(offset);
        assert(nparams >= 0);
        offset++;
        assert((offset + nparams * 2) == numInputs());
        parameterControls_ = mInput + offset;
        numParameterControls_ = nparams;
        LOG_DEBUG("parameter controls: " << nparams);
    }

    // Ugen input/output busses must not be nullptr!
    if (ugenInputs_ && ugenOutputs_){
        // create delegate
        auto mem = RTAlloc(mWorld, sizeof(VSTPluginDelegate));
        if (mem) {
            delegate_.reset(new (mem) VSTPluginDelegate(*this));
            mSpecialIndex |= Valid;
        } else {
            LOG_ERROR("RTAlloc failed!");
        }
    } else {
        LOG_ERROR("RTAlloc failed!");
    }

    // create reblocker (if needed)
    if (valid() && reblockSize > bufferSize()){
        initReblocker(reblockSize);
    }

    // create dummy input/output buffer
    size_t dummyBlocksize = reblock_ ? reblock_->blockSize : bufferSize();
    auto dummyBufsize = dummyBlocksize * 2 * sizeof(float);
    dummyBuffer_ = (float *)RTAlloc(mWorld, dummyBufsize);
    if (dummyBuffer_){
        memset(dummyBuffer_, 0, dummyBufsize); // !
    } else {
        LOG_ERROR("RTAlloc failed!");
        setInvalid();
    }

    // run queued unit commands
    if (mSpecialIndex & UnitCmdQueued) {
        auto item = unitCmdQueue_;
        while (item) {
            if (delegate_){
                sc_msg_iter args(item->size, item->data);
                // swallow the first 3 arguments
                args.geti(); // node ID
                args.geti(); // ugen index
                args.gets(); // unit command name
                (item->fn)((Unit*)this, &args);
            }
            auto next = item->next;
            RTFree(mWorld, item);
            item = next;
        }
    }

    mSpecialIndex |= Initialized; // !

    mCalcFunc = [](Unit *unit, int numSamples){
        static_cast<VSTPlugin *>(unit)->next(numSamples);
    };
    // don't run the calc function, instead just set
    // the first samples of each UGen output to zero
    for (int i = 0; i < numOutputs(); ++i){
        out0(i) = 0;
    }

    LOG_DEBUG("created VSTPlugin instance");
}

VSTPlugin::~VSTPlugin(){
    clearMapping();

    RTFree(mWorld, paramState_);
    RTFree(mWorld, paramMapping_);

    RTFree(mWorld, ugenInputs_);
    RTFree(mWorld, ugenOutputs_);

    // plugin input buffers
    for (int i = 0; i < numPluginInputs_; ++i){
        RTFree(mWorld, pluginInputs_[i].channelData32);
    }
    RTFree(mWorld, pluginInputs_);
    // plugin output buffers
    for (int i = 0; i < numPluginOutputs_; ++i){
        RTFree(mWorld, pluginOutputs_[i].channelData32);
    }
    RTFree(mWorld, pluginOutputs_);

    RTFree(mWorld, dummyBuffer_);

    freeReblocker();

    // tell the delegate that we've been destroyed!
    delegate_->setOwner(nullptr);
    delegate_ = nullptr; // release our reference
    LOG_DEBUG("destroyed VSTPlugin");
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
        } else {
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
    } else {
        return 0.f;
    }
}

bool VSTPlugin::setupBuffers(AudioBus *& pluginBusses, int &pluginBusCount,
                             int& totalNumChannels, Bus *ugenBusses, int ugenBusCount,
                             const int *speakers, int numSpeakers, float *dummy)
{
    // free excess bus channels
    for (int i = numSpeakers; i < pluginBusCount; ++i){
        RTFree(mWorld, pluginBusses[i].channelData32);
        // if the following RTRealloc fails!
        pluginBusses[i].channelData32 = nullptr;
        pluginBusses[i].numChannels = 0;
    }
    AudioBus *result;
    // numSpeakers = 0 has to handled specially!
    if (numSpeakers > 0) {
        result = (AudioBus*)RTRealloc(mWorld,
            pluginBusses, numSpeakers * sizeof(AudioBus));
        if (!result){
            return false; // bail!
        }
    } else {
        RTFree(mWorld, pluginBusses);
        result = nullptr;
    }
    // init new busses, in case a subsequent RTRealloc call fails!
    for (int i = pluginBusCount; i < numSpeakers; ++i) {
        result[i].channelData32 = nullptr;
        result[i].numChannels = 0;
    }
    // now we can update the bus array
    pluginBusses = result;
    pluginBusCount = numSpeakers;
    // (re)allocate plugin busses
    totalNumChannels = 0;
    for (int i = 0; i < numSpeakers; ++i) {
        auto& bus = pluginBusses[i];
        auto channelCount = speakers[i];
        // we only need to update if the channel count has changed!
        if (bus.numChannels != channelCount) {
            if (channelCount > 0) {
                // try to resize array
                auto result = (float**)RTRealloc(mWorld,
                    bus.channelData32, channelCount * sizeof(float *));
                if (!result) {
                    return false; // bail!
                }
                bus.channelData32 = result;
                bus.numChannels = channelCount;
            } else {
                // free old array!
                RTFree(mWorld, bus.channelData32);
                bus.channelData32 = nullptr;
                bus.numChannels = 0;
            }
        }
        totalNumChannels += channelCount;
    }
    // set channels
    assert(ugenBusCount >= 1);
    if (ugenBusCount == 1 && pluginBusCount > 1){
        // distribute ugen channels over plugin busses
        //
        // NOTE: only do this if the plugin has more than one bus,
        // as a workaround for buggy VST3 plugins which would report
        // a wrong default channel count, like Helm.vst3 or RoughRider2.vst3
        auto channels = ugenBusses[0].channelData;
        auto numChannels = ugenBusses[0].numChannels;
        int index = 0;
        for (int i = 0; i < pluginBusCount; ++i) {
            auto& bus = pluginBusses[i];
            for (int j = 0; j < bus.numChannels; ++j, ++index) {
                if (index < numChannels) {
                    bus.channelData32[j] = channels[index];
                } else {
                    // point to dummy buffer!
                    bus.channelData32[j] = dummy;
                }
            }
        }
    } else {
        // associate ugen busses with plugin busses
        for (int i = 0; i < pluginBusCount; ++i) {
            auto& bus = pluginBusses[i];
            auto ugenChannels = i < ugenBusCount ? ugenBusses[i].numChannels : 0;
            for (int j = 0; j < bus.numChannels; ++j) {
                if (j < ugenChannels) {
                    bus.channelData32[j] = ugenBusses[i].channelData[j];
                } else {
                    // point to dummy buffer!
                    bus.channelData32[j] = dummy;
                }
            }
        }
    }
    return true;
}

void VSTPlugin::initReblocker(int reblockSize){
    LOG_DEBUG("reblocking from " << bufferSize()
        << " to " << reblockSize << " samples");
    reblock_ = (Reblock *)RTAlloc(mWorld, sizeof(Reblock));
    if (reblock_){
        memset(reblock_, 0, sizeof(Reblock)); // init!

        // make sure that block size is power of 2
        reblock_->blockSize = NEXTPOWEROFTWO(reblockSize);

        // allocate input/output busses
        // NOTE: we always have at least one input and output bus!
        reblock_->inputs = (Bus*)RTAlloc(mWorld, numUgenInputs_ * sizeof(Bus));
        reblock_->numInputs = reblock_->inputs ? numUgenInputs_ : 0;

        reblock_->outputs = (Bus*)RTAlloc(mWorld, numUgenOutputs_ * sizeof(Bus));
        reblock_->numOutputs = reblock_->outputs ? numUgenOutputs_ : 0;

        if (!(reblock_->inputs && reblock_->outputs)){
            LOG_ERROR("RTAlloc failed!");
            freeReblocker();
            return;
        }

        // set and count channel numbers
        int totalNumChannels = 0;
        for (int i = 0; i < numUgenInputs_; ++i){
            auto numChannels = ugenInputs_[i].numChannels;
            reblock_->inputs[i].numChannels = numChannels;
            reblock_->inputs[i].channelData = nullptr;
            totalNumChannels += numChannels;
        }
        for (int i = 0; i < numUgenOutputs_; ++i){
            auto numChannels = ugenOutputs_[i].numChannels;
            reblock_->outputs[i].numChannels = numChannels;
            reblock_->outputs[i].channelData = nullptr;
            totalNumChannels += numChannels;
        }
        if (totalNumChannels == 0){
            // nothing to do
            return;
        }

        // allocate buffer
        int bufsize = sizeof(float) * totalNumChannels * reblock_->blockSize;
        reblock_->buffer = (float *)RTAlloc(mWorld, bufsize);

        if (reblock_->buffer){
            auto bufptr = reblock_->buffer;
            // zero
            memset(bufptr, 0, bufsize);
            // allocate and assign channel vectors
            auto initBusses = [&](Bus * busses, int count, int blockSize){
                for (int i = 0; i < count; ++i){
                    auto& bus = busses[i];
                    if (bus.numChannels > 0) {
                        bus.channelData = (float**)RTAlloc(mWorld, bus.numChannels * sizeof(float*));
                        if (bus.channelData) {
                            for (int j = 0; j < bus.numChannels; ++j, bufptr += blockSize) {
                                bus.channelData[j] = bufptr;
                            }
                        } else {
                            bus.numChannels = 0; // !
                            return false; // bail
                        }
                    }
                }
                return true;
            };
            if (initBusses(reblock_->inputs, reblock_->numInputs, reblock_->blockSize)
                && initBusses(reblock_->outputs, reblock_->numOutputs, reblock_->blockSize))
            {
                // start phase at one block before end, so that the first call
                // to the perform routine will trigger plugin processing.
                reblock_->phase = reblock_->blockSize - bufferSize();
            } else {
                LOG_ERROR("RTAlloc failed!");
                freeReblocker();
            }
        } else {
            LOG_ERROR("RTAlloc failed!");
            freeReblocker();
        }
    } else {
        LOG_ERROR("RTAlloc failed!");
    }
}

bool VSTPlugin::updateReblocker(int numSamples){
    // read input
    for (int i = 0; i < numUgenInputs_; ++i){
        auto& inputs = ugenInputs_[i];
        auto reblockInputs = reblock_->inputs[i].channelData;
        for (int j = 0; j < inputs.numChannels; ++j){
            auto src = inputs.channelData[j];
            auto dst = reblockInputs[j] + reblock_->phase;
            std::copy(src, src + numSamples, dst);
        }
    }

    reblock_->phase += numSamples;

    if (reblock_->phase >= reblock_->blockSize){
        assert(reblock_->phase == reblock_->blockSize);
        reblock_->phase = 0;
        return true;
    } else {
        return false;
    }
}

void VSTPlugin::freeReblocker(){
    if (reblock_){
        for (int i = 0; i < reblock_->numInputs; ++i){
            RTFree(mWorld, reblock_->inputs[i].channelData);
        }
        for (int i = 0; i < reblock_->numOutputs; ++i){
            RTFree(mWorld, reblock_->outputs[i].channelData);
        }
        RTFree(mWorld, reblock_->inputs);
        RTFree(mWorld, reblock_->outputs);
        RTFree(mWorld, reblock_);
    }
}

// update data (after loading a new plugin)
void VSTPlugin::setupPlugin(const int *inputs, int numInputs,
                            const int *outputs, int numOutputs)
{
    delegate().update();

    auto inDummy = dummyBuffer_;
    auto outDummy = dummyBuffer_ +
            (reblock_ ? reblock_->blockSize : bufferSize());

    // setup buffers
    if (reblock_){
        if (!setupBuffers(pluginInputs_, numPluginInputs_,
                          numPluginInputChannels_,
                          reblock_->inputs, reblock_->numInputs,
                          inputs, numInputs, inDummy) ||
            !setupBuffers(pluginOutputs_, numPluginOutputs_,
                          numPluginOutputChannels_,
                          reblock_->outputs, reblock_->numOutputs,
                          outputs, numOutputs, outDummy))
        {
            LOG_ERROR("RTRealloc failed!");
            setInvalid();
        }
    } else {
        if (!setupBuffers(pluginInputs_, numPluginInputs_,
                          numPluginInputChannels_,
                          ugenInputs_, numUgenInputs_,
                          inputs, numInputs, inDummy) ||
            !setupBuffers(pluginOutputs_, numPluginOutputs_,
                          numPluginOutputChannels_,
                          ugenOutputs_, numUgenOutputs_,
                          outputs, numOutputs, outDummy))
        {
            LOG_ERROR("RTRealloc failed!");
            setInvalid();
        }
    }

    clearMapping();

    // parameter states
    int numParams = delegate().plugin()->info().numParameters();
    if (numParams > 0) {
        auto result = (float*)RTRealloc(mWorld,
            paramState_, numParams * sizeof(float));
        if (result) {
            for (int i = 0; i < numParams; ++i) {
            #if 0
                // breaks floating point comparison on GCC with -ffast-math
                result[i] = std::numeric_limits<float>::quiet_NaN();
            #else
                result[i] = std::numeric_limits<float>::max();
            #endif
            }
            paramState_ = result;
        } else {
            LOG_ERROR("RTRealloc failed!");
            setInvalid();
        }
    } else {
        RTFree(mWorld, paramState_);
        paramState_ = nullptr;
    }

    // parameter mapping
    if (numParams > 0){
        auto result = (Mapping **)RTRealloc(mWorld,
            paramMapping_, numParams * sizeof(Mapping*));
        if (result) {
            for (int i = 0; i < numParams; ++i) {
                result[i] = nullptr;
            }
            paramMapping_ = result;
        } else {
            LOG_ERROR("RTRealloc failed!");
            setInvalid();
        }
    } else {
        RTFree(mWorld, paramMapping_);
        paramMapping_ = nullptr;
    }
}

#if 1
void VSTPlugin::printMapping(){
    LOG_DEBUG("mappings:");
    for (auto mapping = paramMappingList_; mapping; mapping = mapping->next){
        LOG_DEBUG(mapping->index << " -> " << mapping->bus() << " (" << mapping->type() << ")");
    }
}
#else
void VSTPlugin::printMapping() {}
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
        } else {
            LOG_ERROR("RTAlloc failed!");
            return;
        }
    }
    mapping->setBus(bus, audio ? Mapping::Audio : Mapping::Control);
    printMapping();
}

void VSTPlugin::unmap(int32 index) {
    auto mapping = paramMapping_[index];
    if (mapping) {
        // remove from linked list
        if (mapping->prev) {
            mapping->prev->next = mapping->next;
        } else { // head
            paramMappingList_ = mapping->next;
        }
        if (mapping->next) {
            mapping->next->prev = mapping->prev;
        }
        RTFree(mWorld, mapping);
        paramMapping_[index] = nullptr;
    }
    printMapping();
}

// perform routine
void VSTPlugin::next(int inNumSamples) {
#ifdef SUPERNOVA
    // with Supernova the "next" routine might be called in different threads - each time!
    setCurrentThreadRT();
#endif
    if (!valid()){
        ClearUnitOutputs(this, inNumSamples);
        return;
    }

    auto plugin = delegate_->plugin();
    bool process = plugin && plugin->info().hasPrecision(ProcessPrecision::Single);

    // Whenever an asynchronous command is executing, the plugin is temporarily
    // suspended. This is mainly for blocking other commands until the async
    // command has finished, but it also means we only have to lock if we're
    // suspended. The actual critical section is protected by a spinlock.
    std::unique_lock<SpinLock> lock;
    if (delegate_->isSuspended()) {
        // We try to lock and bypass on failure so we don't block the whole Server.
        lock = delegate_->tryLock();
        if (process && !lock) {
            LOG_DEBUG("VSTPlugin: couldn't lock mutex");
            process = false;
        }
    }

    if (process) {
        auto vst3 = plugin->info().type() == PluginType::VST3;

        // check bypass state
        Bypass bypass;
        int inBypass = getBypass();
        if (inBypass > 1) {
            bypass = Bypass::Soft;
        } else if (inBypass == 1){
            bypass = Bypass::Hard;
        } else {
            bypass = Bypass::Off;
        }
        if (bypass != bypass_) {
            plugin->setBypass(bypass);
            bypass_ = bypass;
        }

        // parameter automation
        // (check paramState_ in case RTAlloc failed)
        if (paramState_) {
            int sampleOffset = reblockPhase();
            // automate parameters with mapped control busses
            int nparams = plugin->info().numParameters();
            for (auto m = paramMappingList_; m != nullptr; m = m->next) {
                uint32 index = m->index;
                auto type = m->type();
                uint32 num = m->bus();
                assert(index < nparams);
                if (type == Mapping::Control) {
                    // Control Bus mapping
                    float value = readControlBus(num);
                    if (value != paramState_[index]) {
                        plugin->setParameter(index, value, sampleOffset);
                        paramState_[index] = value;
                    }
                } else if (num < mWorld->mNumAudioBusChannels){
                    // Audio Bus mapping
                #define unit this
                    float last = paramState_[index];
                    float* bus = &mWorld->mAudioBus[mWorld->mBufLength * num];
                    ACQUIRE_BUS_AUDIO_SHARED(num);
                    if (vst3) {
                        // VST3: sample accurate
                        for (int i = 0; i < inNumSamples; ++i) {
                            float value = bus[i];
                            if (value != last) {
                                plugin->setParameter(index, value, sampleOffset + i);
                                last = value;
                            }
                        }
                    } else {
                        // VST2: pick the first sample
                        float value = *bus;
                        if (value != last) {
                            plugin->setParameter(index, value); // no offset
                            last = value;
                        }
                    }
                    RELEASE_BUS_AUDIO_SHARED(num);
                    paramState_[index] = last;
                #undef unit
                }
            }
            // automate parameters with UGen inputs
            auto numControls = numParameterControls_;
            for (int i = 0; i < numControls; ++i) {
                auto control = parameterControls_ + i * 2;
                int index = control[0]->mBuffer[0];
                // only if index is not out of range and the parameter is not mapped to a bus
                // (a negative index effectively deactivates the parameter control)
                if (index >= 0 && index < nparams && paramMapping_[index] == nullptr){
                    auto calcRate = control[1]->mCalcRate;
                    auto buffer = control[1]->mBuffer;
                    if (calcRate == calc_FullRate) {
                        // audio rate
                        float last = paramState_[index];
                        // VST3: sample accurate
                        if (vst3) {
                            for (int i = 0; i < inNumSamples; ++i) {
                                float value = buffer[i];
                                if (value != last) {
                                    plugin->setParameter(index, value, sampleOffset + i);
                                    last = value;
                                }
                            }
                        } else {
                            // VST2: pick the first sample
                            float value = buffer[0];
                            if (value != last) {
                                plugin->setParameter(index, value); // no offset
                                last = value;
                            }
                        }
                        paramState_[index] = last;
                    } else {
                        // control rate
                        float value = buffer[0];
                        if (value != paramState_[index]) {
                            plugin->setParameter(index, value, sampleOffset);
                            paramState_[index] = value;
                        }
                    }
                }
            }
        }
        // process
        ProcessData data;
        data.precision = ProcessPrecision::Single;
        data.mode = mWorld->mRealTime ? ProcessMode::Realtime : ProcessMode::Offline;
        data.numInputs = numPluginInputs_;
        data.inputs = pluginInputs_;
        data.numOutputs = numPluginOutputs_;
        data.outputs = pluginOutputs_;

        if (reblock_){
            if (updateReblocker(inNumSamples)){
                data.numSamples = reblock_->blockSize;

                plugin->process(data);
            }

            // write reblocker output
            for (int i = 0; i < numUgenOutputs_ && i < numPluginOutputs_; ++i){
                int ugenChannels = ugenOutputs_[i].numChannels;
                int pluginChannels = pluginOutputs_[i].numChannels;
                for (int j = 0; j < ugenChannels && j < pluginChannels; ++j){
                    auto src = reblock_->outputs[i].channelData[j] + reblock_->phase;
                    auto dst = ugenOutputs_[i].channelData[j];
                    std::copy(src, src + inNumSamples, dst);
                }
            }
        } else {
            data.numSamples = inNumSamples;

            plugin->process(data);
        }

        // see VSTPluginDelegate::setParam(), setProgram and parameterAutomated()
        delegate().isSettingParam_ = false;
        delegate().isSettingProgram_ = false;

        // handle deferred parameter updates
        if (delegate().paramBitset_ && paramState_) {
            auto bitset = delegate().paramBitset_;
            auto size = delegate().paramBitsetSize_;
            auto numbits = VSTPluginDelegate::paramNumBits;
            auto threaded = delegate().threaded_;
            // NB: if threaded, dispatch *previous* param changes
            auto paramChange = threaded ? bitset + size : bitset;
            for (int i = 0; i < size; ++i) {
                if (paramChange[i].any()) {
                    auto numParams = plugin->info().numParameters();
                    for (int j = 0; j < numbits; ++j) {
                        if (paramChange[i].test(j)) {
                            // cache and send parameter
                            // NB: we need to check the parameter count! See update()
                            auto index = i * numbits + j;
                            if (index < numParams) {
                                auto value = plugin->getParameter(index);
                                paramState_[index] = value;
                                delegate().sendParameter(index, value);
                            }
                        }
                    }
                    // clear bitset!
                    paramChange[i].reset();
                }
            }
            if (threaded) {
                // check *new* parameter changes.
                // NB: if any parameter causes outgoing parameter changes, these will
                // be sent in the *next* process function call, that's why we set
                // 'isSettingParam_' again.
                auto newParamChange = bitset;
                if (std::any_of(newParamChange, newParamChange + size,
                                [](auto& x) { return x.any(); })) {
                    delegate().isSettingParam_ = true;
                }
                // finally, swap bitsets
                std::swap_ranges(newParamChange, paramChange, paramChange);
                // all bits should be zero now!
                assert(std::all_of(newParamChange, newParamChange + size,
                                   [](auto& x) { return x.none(); }));
            }
        }

        // zero remaining Ugen outputs
        if (numUgenOutputs_ == 1){
            // plugin outputs might be destributed
            auto& ugenOutputs = ugenOutputs_[0];
            for (int i = numPluginOutputChannels_; i < ugenOutputs.numChannels; ++i){
                auto out = ugenOutputs.channelData[i];
                std::fill(out, out + inNumSamples, 0);
            }
        } else {
            for (int i = 0; i < numUgenOutputs_; ++i){
                auto& ugenOutputs = ugenOutputs_[i];
                int onset = (i < numPluginOutputs_) ?
                    pluginOutputs_[i].numChannels : 0;
                for (int j = onset; j < ugenOutputs.numChannels; ++j){
                    auto out = ugenOutputs.channelData[j];
                    std::fill(out, out + inNumSamples, 0);
                }
            }
        }

        // send parameter automation notification posted from the GUI thread [or NRT thread]
        delegate().handleEvents();
    } else {
        // bypass
        if (reblock_){
            // we have to update the reblocker, so that we can stop bypassing
            // anytime and always have valid input data.
            updateReblocker(inNumSamples);

            performBypass(reblock_->inputs, reblock_->numInputs, inNumSamples, reblock_->phase);
        } else {
            performBypass(ugenInputs_, numUgenInputs_, inNumSamples, 0);
        }
    }
}

void VSTPlugin::performBypass(const Bus *ugenInputs, int numInputs,
                              int numSamples, int phase)
{
    for (int i = 0; i < numUgenOutputs_; ++i){
        auto& outputs = ugenOutputs_[i];
        if (i < numInputs){
            auto& inputs = ugenInputs[i];
            for (int j = 0; j < outputs.numChannels; ++j){
                if (j < inputs.numChannels){
                    // copy input to output
                    auto chn = inputs.channelData[j] + phase;
                    std::copy(chn, chn + numSamples, outputs.channelData[j]);
                } else {
                    // zero outlet
                    auto chn = outputs.channelData[j];
                    std::fill(chn, chn + numSamples, 0);
                }
            }
        } else {
            // zero whole bus
            for (int j = 0; j < outputs.numChannels; ++j){
                auto chn = outputs.channelData[j];
                std::fill(chn, chn + numSamples, 0);
            }
        }
    }
}

int VSTPlugin::blockSize() const {
    return reblock_ ? reblock_->blockSize : bufferSize();
}

int VSTPlugin::reblockPhase() const {
    return reblock_ ? reblock_->phase : 0;
}

//------------------- VSTPluginDelegate ------------------------------//

VSTPluginDelegate::VSTPluginDelegate(VSTPlugin& owner) {
    setOwner(&owner);
    auto queue = (ParamQueue*)RTAlloc(world(), sizeof(ParamQueue));
    if (queue) {
        paramQueue_ = new (queue) ParamQueue();
    } else {
        paramQueue_ = nullptr;
        LOG_ERROR("RTAlloc failed!");
    }
}

VSTPluginDelegate::~VSTPluginDelegate() {
    assert(plugin_ == nullptr);

    if (paramQueue_) {
        if (paramQueue_->needRelease()) {
            // release internal memory on the NRT thread,
            // but param queue itself on the RT thread.
            DoAsynchronousCommand(world(), 0, 0, paramQueue_,
                [](World *, void *inData) {
                    static_cast<ParamQueue*>(inData)->release();
                    return false;
                }, nullptr, nullptr, cmdRTfree<ParamQueue>, 0, 0);
        } else {
            // no internal memory, free immediately on the RT thread.
            paramQueue_->~ParamQueue();
            RTFree(world(), paramQueue_);
        }
    }

    if (paramBitset_) {
        RTFree(world(), paramBitset_);
    }

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
    if (isCurrentThreadRT()) {
        // Only send outgoing parameter changes if caused by /set or /setn!
        // Ignore parameter changes caused by UGen input automation, control/audio bus
        // mappings or program changes to prevent flooding the client with messages.
        //
        // NB: we can only unset 'isSettingParam_' at the end of theVSTPlugin::next()
        // function because outgoing parameter changes might be sent in the plugin
        // process function, e.g. with VST3 plugins or bridged plugins.
        // Unfortunately, this allows other parameter changes to pass through for
        // the duration of that block. I don't see a real solution for this...
        // In practice, the generic UI and UGen input automation are at odds anyway.
        if (isSettingParam_ && !isSettingProgram_) {
            sendParameterAutomated(index, value);
        }
    } else if (paramQueue_) {
        // from UI/NRT thread -> push to queue
        // Ignore if sent as a result of reading program/bank data! See comment above.
        if (!isSettingState_) {
            paramQueue_->emplace(index, value); // thread-safe!
        }
    }
}

void VSTPluginDelegate::latencyChanged(int nsamples){
    if (isCurrentThreadRT()) {
        sendLatencyChange(nsamples);
    } else if (paramQueue_) {
        // from UI/NRT thread - push to queue
        paramQueue_->emplace(LatencyChange, (float)nsamples); // thread-safe!
    }
}

void VSTPluginDelegate::updateDisplay() {
    if (isCurrentThreadRT()) {
        sendUpdateDisplay();
    } else if (paramQueue_) {
        // from UI/NRT thread - push to queue
        paramQueue_->emplace(UpdateDisplay, 0.f); // thread-safe!
    }
}

void VSTPluginDelegate::pluginCrashed(){
    // From the watch dog thread
    if (paramQueue_) {
        paramQueue_->emplace(PluginCrash, 0.f); // thread-safe!
    }
}

void VSTPluginDelegate::midiEvent(const MidiEvent& midi) {
    // so far, we only handle MIDI events that come from the RT thread
    if (isCurrentThreadRT()) {
        float buf[3];
        // we don't want negative values here
        buf[0] = (unsigned char)midi.data[0];
        buf[1] = (unsigned char)midi.data[1];
        buf[2] = (unsigned char)midi.data[2];
        sendMsg("/vst_midi", 3, buf);
    }
}

void VSTPluginDelegate::sysexEvent(const SysexEvent & sysex) {
    // so far, we only handle SysEx events that come from the RT thread
    if (isCurrentThreadRT()) {
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

void VSTPluginDelegate::update(){
    if (paramQueue_) paramQueue_->clear();

    isSettingParam_ = false; // just to be sure
    isSettingProgram_ = false;

    if (paramBitset_) {
        RTFree(world(),paramBitset_);
        paramBitset_ = nullptr;
        paramBitsetSize_ = 0;
    }
    // allocate parameter bitset if plugin processing is deferred
    auto numParams = plugin()->info().numParameters();
    if (numParams > 0 && plugin()->isBridged() || plugin()->isThreaded()) {
        auto d = std::div(numParams, paramNumBits);
        auto size = d.quot + (d.rem > 0);
        // threaded plugin needs twice the size for double buffering
        auto realSize = plugin()->isThreaded() ? size * 2 : size;
        auto bitset = (ParamBitset *)RTAlloc(world(), realSize * sizeof(ParamBitset));
        if (bitset) {
            for (int i = 0; i < realSize; ++i) {
                new (&bitset[i]) ParamBitset{};
            }
            paramBitset_ = bitset;
            paramBitsetSize_ = size;
        } else {
            LOG_ERROR("RTAlloc failed!");
        }
    }
}

void VSTPluginDelegate::handleEvents(){
    // TODO: rate limit?
    if (paramQueue_) {
        ParamChange p;
        while (paramQueue_->pop(p)){
            if (p.index >= 0){
                sendParameterAutomated(p.index, p.value);
            } else if (p.index == VSTPluginDelegate::LatencyChange){
                sendLatencyChange(p.value);
            } else if (p.index == VSTPluginDelegate::UpdateDisplay){
                sendUpdateDisplay();
            } else if (p.index == VSTPluginDelegate::PluginCrash){
                sendPluginCrash();
            }
        }
    }
}

// try to close the plugin in the NRT thread with an asynchronous command
void VSTPluginDelegate::close() {
    if (!check()) return;
    LOG_DEBUG("about to close");
    doClose();
}

void VSTPluginDelegate::doClose(){
    if (plugin_){
        auto cmdData = CmdData::create<CloseCmdData>(world());
        if (!cmdData) {
            return;
        }
        cmdData->plugin = std::move(plugin_);
        cmdData->editor = editor_;
        // NOTE: the plugin might send an event between here and
        // the NRT stage, e.g. when automating parameters in the
        // plugin UI. Since the events come from the UI thread,
        // we must not unset the listener in the audio thread,
        // otherwise we have a race condition.
        // Instead, we keep the delegate alive until the plugin
        // has been closed. See VSTPluginDelegate::release()
    #if 0
        data->plugin->setListener(nullptr);
    #endif
        doCmd(cmdData, [](World *world, void* inData) {
            auto data = (CloseCmdData*)inData;
            // release plugin on the correct thread
            defer([&](){
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
    // check if RTAlloc failed
    if (!data->inputs || !data->outputs){
        return true; // continue
    }
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
                LOG_DEBUG("create plugin");
                data->plugin = info->create(data->editor, data->threaded, data->runMode);
                // setup plugin
                LOG_DEBUG("suspend");
                data->plugin->suspend();
                if (info->hasPrecision(ProcessPrecision::Single)) {
                    LOG_DEBUG("setupProcessing ("
                              << ((data->processMode == ProcessMode::Realtime) ? "realtime" : "offline")
                              << ")");
                    data->plugin->setupProcessing(data->sampleRate, data->blockSize,
                                                  ProcessPrecision::Single, data->processMode);
                } else {
                    LOG_WARNING("VSTPlugin: plugin '" << info->name <<
                                "' doesn't support single precision processing - bypassing!");
                }
                LOG_DEBUG("setNumSpeakers");

                auto setupSpeakers = [](const auto& pluginBusses,
                        const int *ugenBusses, int numUgenBusses,
                        auto& result, const char *what){
                    assert(numUgenBusses >= 1);
                    result.resize(pluginBusses.size());

                    if (numUgenBusses == 1 && pluginBusses.size() > 1){
                        LOG_DEBUG("distribute ugen " << what);
                        // distribute ugen channels over plugin busses
                        //
                        // NOTE: only do this if the plugin has more than one bus,
                        // as a workaround for buggy VST3 plugins which would report
                        // a wrong default channel count, like Helm.vst3 or RoughRider2.vst3
                        auto remaining = ugenBusses[0];
                        for (int i = 0; i < (int)pluginBusses.size(); ++i){
                            if (remaining > 0){
                                auto chn = std::min<int>(remaining, pluginBusses[i].numChannels);
                                result[i] = chn;
                                remaining -= chn;
                            } else {
                                result[i] = 0;
                            }
                        }
                    } else {
                        LOG_DEBUG("associate ugen " << what);
                        // associate ugen input/output busses with plugin input/output busses.
                        for (int i = 0; i < (int)pluginBusses.size(); ++i){
                            if (i < numUgenBusses){
                                result[i] = ugenBusses[i];
                            } else {
                                result[i] = 0;
                            }
                        }
                    }
                };

                // prepare input busses
                setupSpeakers(data->plugin->info().inputs, data->inputs, data->numInputs,
                              data->pluginInputs, "inputs");
                // prepare output busses
                setupSpeakers(data->plugin->info().outputs, data->outputs, data->numOutputs,
                              data->pluginOutputs, "outputs");

                data->plugin->setNumSpeakers(data->pluginInputs.data(), data->pluginInputs.size(),
                                             data->pluginOutputs.data(), data->pluginOutputs.size());

                LOG_DEBUG("resume");
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
        LOG_WARNING("VSTPlugin: already loading!");
        sendMsg("/vst_open", 0);
        return;
    }
    if (suspended_){
        LOG_WARNING("VSTPlugin: temporarily suspended!");
        sendMsg("/vst_open", 0);
        return;
    }
    doClose();
    if (plugin_) {
        // shouldn't happen...
        LOG_ERROR("couldn't close current plugin!");
        sendMsg("/vst_open", 0);
        return;
    }
#ifdef SUPERNOVA
    if (threaded){
        LOG_WARNING("multiprocessing option ignored on Supernova!");
        threaded = false;
    }
#endif

    auto len = strlen(path) + 1;
    auto cmdData = CmdData::create<OpenCmdData>(world(), len);
    if (cmdData) {
        memcpy(cmdData->path, path, len);
        cmdData->editor = editor;
        cmdData->threaded = threaded;
        cmdData->runMode = mode;
        cmdData->sampleRate = owner_->sampleRate();
        cmdData->blockSize = owner_->blockSize();
        cmdData->processMode = world_->mRealTime ? ProcessMode::Realtime : ProcessMode::Offline;
        // copy ugen input busses
        assert(owner_->numInputBusses() > 0);
        cmdData->numInputs = owner_->numInputBusses();
        cmdData->inputs = (int *)RTAlloc(world_, cmdData->numInputs * sizeof(int));
        if (cmdData->inputs){
            for (int i = 0; i < cmdData->numInputs; ++i){
                cmdData->inputs[i] = owner_->inputBusses()[i].numChannels;
            }
        } else {
            cmdData->numInputs = 0;
            LOG_ERROR("RTAlloc failed!");
        }
        // copy ugen outputs busses
        assert(owner_->numOutputBusses() > 0);
        cmdData->numOutputs = owner_->numOutputBusses();
        cmdData->outputs = (int *)RTAlloc(world_, cmdData->numOutputs * sizeof(int));
        if (cmdData->outputs){
            for (int i = 0; i < cmdData->numOutputs; ++i){
                cmdData->outputs[i] = owner_->outputBusses()[i].numChannels;
            }
        } else {
            cmdData->numOutputs = 0;
            LOG_ERROR("RTAlloc failed!");
        }

        doCmd(cmdData, cmdOpen,
            [](World *world, void *cmdData){
                auto data = (OpenCmdData*)cmdData;
                data->owner->doneOpen(*data); // alive() checked in doneOpen!
                return true; // continue
            },
            [](World *world, void *cmdData){
                auto data = (OpenCmdData*)cmdData;
                // free vectors in NRT thread!
                data->pluginInputs = std::vector<int>{};
                data->pluginOutputs = std::vector<int>{};
                return false; // done
            }
        );

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
    // move *before* calling alive(), so that doClose() can close it.
    plugin_ = std::move(cmd.plugin);
    if (!alive()) {
        LOG_WARNING("VSTPlugin freed during 'open'");
        // properly release the plugin
        doClose();
        return; // !
    }
    if (plugin_){
        if (!plugin_->info().hasPrecision(ProcessPrecision::Single)) {
            LOG_WARNING("'" << plugin_->info().name
                        << "' doesn't support single precision processing - bypassing!");
        }
        LOG_DEBUG("opened " << cmd.path);
        // setup data structures
        owner_->setupPlugin(cmd.pluginInputs.data(), cmd.pluginInputs.size(),
                            cmd.pluginOutputs.data(), cmd.pluginOutputs.size());
        // receive events from plugin
        plugin_->setListener(this);
        // success, window, initial latency
        bool haveWindow = plugin_->getWindow() != nullptr;
        int latency = plugin_->getLatencySamples() + latencySamples();
        float data[3] = { 1.f, (float)haveWindow, (float)latency };
        sendMsg("/vst_open", 3, data);
    } else {
        LOG_WARNING("VSTPlugin: couldn't open " << cmd.path);
        sendMsg("/vst_open", 0);
    }

    // RTAlloc might have failed!
    RTFree(world_, cmd.inputs);
    RTFree(world_, cmd.outputs);
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
                } else {
                    window->close();
                }
                return false; // done
            });
        }
    }
}

void VSTPluginDelegate::setEditorPos(int x, int y) {
    if (plugin_ && plugin_->getWindow()) {
        auto cmdData = CmdData::create<WindowCmdData>(world());
        if (cmdData) {
            cmdData->x = x;
            cmdData->y = y;
            doCmd(cmdData, [](World * inWorld, void* inData) {
                auto data = (WindowCmdData*)inData;
                auto window = data->owner->plugin()->getWindow();
                window->setPos(data->x, data->y);
                return false; // done
            });
        }
    }
}

void VSTPluginDelegate::setEditorSize(int w, int h){
    if (plugin_ && plugin_->getWindow()) {
        auto cmdData = CmdData::create<WindowCmdData>(world());
        if (cmdData) {
            cmdData->width = w;
            cmdData->height = h;
            doCmd(cmdData, [](World * inWorld, void* inData) {
                auto data = (WindowCmdData*)inData;
                auto window = data->owner->plugin()->getWindow();
                window->setSize(data->width, data->height);
                return false; // done
            });
        }
    }
}

void VSTPluginDelegate::doReset() {
    ScopedNRTLock lock(spinMutex_);
    plugin_->suspend();
    plugin_->resume();
}

void VSTPluginDelegate::reset(bool async) {
    if (check()) {
    #if 1
        // force async if we have a plugin UI to avoid
        // race conditions with concurrent UI updates.
        if (editor_ && !async){
            LOG_VERBOSE("'async' can't be 'false' when using the VST editor");
            async = true;
        }
    #endif
        if (async) {
            // reset in the NRT thread
            suspend(); // suspend
            doCmd(CmdData::create<PluginCmdData>(world()),
                [](World *world, void *cmdData){
                    auto data = (PluginCmdData *)cmdData;
                    defer([&](){
                        data->owner->doReset();
                    }, data->owner->hasEditor());
                    return true; // continue
                },
                [](World *world, void *cmdData){
                    auto data = (PluginCmdData *)cmdData;
                    if (!data->alive()) return false;
                    data->owner->resume();
                    return false; // done
                }
            );
        } else {
            // reset in the RT thread
            doReset();
        }
    }
}

void VSTPluginDelegate::setParam(int32 index, float value) {
    if (check()){
        if (index >= 0 && index < plugin_->info().numParameters()) {
            isSettingParam_ = true; // see parameterAutomated()
            int sampleOffset = owner_->mWorld->mSampleOffset + owner_->reblockPhase();
            plugin_->setParameter(index, value, sampleOffset);
            if (paramBitset_) {
                // defer! set corresponding bit in parameter bitset
                auto i = (uint64_t)index / paramNumBits;
                auto j = (uint64_t)index % paramNumBits;
                assert(i >= 0 && i < paramBitsetSize_);
                paramBitset_[i].set(j);

            } else {
                // cache and send immediately; use actual value!
                float newValue = plugin_->getParameter(index);
                owner_->paramState_[index] = newValue;
                sendParameter(index, newValue);
            }
            // NB: isSettingsParam_ will be unset in VSTPlugin::next()!
            owner_->unmap(index);
        } else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::setParam(int32 index, const char* display) {
    if (check()){
        if (index >= 0 && index < plugin_->info().numParameters()) {
            isSettingParam_ = true; // see parameterAutomated()
            int sampleOffset = owner_->mWorld->mSampleOffset + owner_->reblockPhase();
            if (!plugin_->setParameter(index, display, sampleOffset)) {
                LOG_WARNING("VSTPlugin: couldn't set parameter " << index << " to " << display);
                // NB: some plugins don't just ignore bad string input, but reset the parameter to some value...
            }
            if (paramBitset_) {
                // defer! set corresponding bit in parameter bitset
                auto i = (uint64_t)index / paramNumBits;
                auto j = (uint64_t)index % paramNumBits;
                assert(i >= 0 && i < paramBitsetSize_);
                paramBitset_[i].set(j);
            } else {
                // cache and send immediately
                float newValue = plugin_->getParameter(index);
                owner_->paramState_[index] = newValue;
                sendParameter(index, newValue);
            }
            // NB: isSettingsParam_ will be unset in VSTPlugin::next()!
            owner_->unmap(index);
        } else {
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
        } else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::getParam(int32 index) {
    float msg[2] = { (float)index, 0.f };

    if (check()) {
        if (index >= 0 && index < plugin_->info().numParameters()) {
            msg[1] = plugin_->getParameter(index); // value
        } else {
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
            const int nargs = count + 2; // for index + count
            if (nargs * sizeof(float) < MAX_OSC_PACKET_SIZE){
                float *buf = (float *)alloca(sizeof(float) * nargs);
                buf[0] = index;
                buf[1] = count;
                for (int i = 0; i < count; ++i) {
                    float value = plugin_->getParameter(i + index);
                    buf[i + 2] = value;
                }
                sendMsg("/vst_setn", nargs, buf);
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
        } else {
            LOG_WARNING("VSTPlugin: parameter index " << index << " out of range!");
        }
    }
}

void VSTPluginDelegate::unmapParam(int32 index) {
    if (check()) {
        if (index >= 0 && index < plugin_->info().numParameters()) {
            owner_->unmap(index);
        } else {
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
            isSettingProgram_ = true;
            plugin_->setProgram(index);
            // NB: isSettingProgram_ will be unset in VSTPlugin::next()
        } else {
            LOG_WARNING("VSTPlugin: program number " << index << " out of range!");
        }
#if 0
        // don't do this, the program has been actively set by the user!
        sendMsg("/vst_program_index", plugin_->getProgram());
#endif
    }
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

void VSTPluginDelegate::doReadPreset(const std::string& data, bool bank) {
    // NB: readProgramData() can throw, hence the scope guard!
    isSettingState_ = true;
    ScopeGuard guard([this]() {
        isSettingState_ = false;
    });

    ScopedNRTLock lock(spinMutex_);
    if (bank)
        plugin_->readBankData(data);
    else
        plugin_->readProgramData(data);
}

template<bool bank>
bool cmdReadPreset(World* world, void* cmdData) {
    auto data = (PresetCmdData*)cmdData;
    auto& buffer = data->buffer;
    bool async = data->async;
    bool result = true;
    try {
        if (data->bufnum < 0) {
            // from file
            vst::File file(data->path);
            if (file.is_open()){
                LOG_DEBUG("opened preset file " << data->path);
            } else {
                throw Error("couldn't open file " + std::string(data->path));
            }
            file.seekg(0, std::ios_base::end);
            buffer.resize(file.tellg());
            file.seekg(0, std::ios_base::beg);
            file.read(&buffer[0], buffer.size());
            if (file){
                LOG_DEBUG("successfully read " << buffer.size() << " bytes");
            } else {
                throw Error("couldn't read preset data");
            }
        } else {
            // from buffer
            auto sndbuf = World_GetNRTBuf(world, data->bufnum);
            writeBuffer(sndbuf, buffer);
        }
        if (async) {
            // load preset now
            // NOTE: we avoid readProgram() to minimize the critical section
            defer([&]() {
                data->owner->doReadPreset(buffer, bank);
            }, data->owner->hasEditor());
        }
    } catch (const Error& e) {
        LOG_ERROR("couldn't read " << (bank ? "bank: " : "program: ") << e.what());
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
        // TODO: this should probably be deprecated...
        try {
            data->owner->doReadPreset(data->buffer, bank);
        } catch (const Error& e){
            LOG_ERROR("couldn't read " << (bank ? "bank: " : "program: ") << e.what());
            data->result = 0;
        }
    }

    if (bank) {
        owner->sendMsg("/vst_bank_read", data->result);
        // a bank change also sets the current program number!
        owner->sendMsg("/vst_program_index", owner->plugin()->getProgram());
    } else {
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
        if (editor_ && !async){
            LOG_VERBOSE("'async' can't be 'false' when using the VST editor");
            async = true;
        }
    #endif
        if (async){
            suspend();
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

void VSTPluginDelegate::doWritePreset(std::string& buffer, bool bank) {
    ScopedNRTLock lock(spinMutex_);
    if (bank)
        plugin_->writeBankData(buffer);
    else
        plugin_->writeProgramData(buffer);
}

template<bool bank>
bool cmdWritePreset(World *world, void *cmdData) {
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
                data->owner->doWritePreset(buffer, bank);
            }, data->owner->hasEditor());
        }
        if (data->bufnum < 0) {
            // write data to file
            vst::File file(data->path, File::WRITE);
            if (file.is_open()){
                LOG_DEBUG("opened preset file " << data->path);
            } else {
                throw Error("couldn't create file " + std::string(data->path));
            }
            file.write(buffer.data(), buffer.size());
            file.flush();
            if (file){
                LOG_DEBUG("successfully wrote " << buffer.size() << " bytes");
            } else {
                throw Error("couldn't write preset data");
            }
        } else {
            // to buffer
            auto sndbuf = World_GetNRTBuf(world, data->bufnum);
            // free old buffer data in stage 4.
            // usually, the buffer should be already empty.
            data->freeData = sndbuf->data;
            allocReadBuffer(sndbuf, buffer);
        }
    } catch (const Error & e) {
        LOG_ERROR("couldn't write " << (bank ? "bank: " : "program: ") << e.what());
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
    #if 1
        // force async if we have a plugin UI to avoid
        // race conditions with concurrent UI updates.
        if (editor_ && !async){
            LOG_VERBOSE("'async' can't be 'false' when using the VST editor");
            async = true;
        }
    #endif
        auto data = PresetCmdData::create(world(), dest, async);
        if (async){
            suspend();
        } else {
            // TODO: this should probably be deprecated...
            try {
                doWritePreset(data->buffer, bank);
            } catch (const Error& e){
                LOG_ERROR("couldn't write " << (bank ? "bank: " : "program: ") << e.what());
                goto fail;
            }
        }
        doCmd(data, cmdWritePreset<bank>, cmdWritePresetDone<bank>, PresetCmdData::nrtFree);
    } else {
fail:
        if (bank) {
            sendMsg("/vst_bank_write", 0);
        } else {
            sendMsg("/vst_program_write", 0);
        }
    }
}

// midi
void VSTPluginDelegate::sendMidiMsg(int32 status, int32 data1, int32 data2, float detune) {
    if (check()) {
        int sampleOffset = owner_->mWorld->mSampleOffset + owner_->reblockPhase();
        plugin_->sendMidiEvent(MidiEvent(status, data1, data2, sampleOffset, detune));
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
    defer([&](){
        data->index = data->owner->plugin()->vendorSpecific(data->index, data->value,
                                                            data->data, data->opt);
    }, data->owner->hasEditor());
    return true;
}

bool cmdVendorSpecificDone(World *world, void *cmdData) {
    auto data = (VendorCmdData *)cmdData;
    if (!data->alive()) return false;
    data->owner->resume(); // resume
    data->owner->sendMsg("/vst_vendor_method", (float)(data->index));
    return false; // done
}

void VSTPluginDelegate::vendorSpecific(int32 index, int32 value, size_t size,
                                       const char *data, float opt, bool async) {
    if (check()) {
        // some calls might be safe to do on the RT thread
        // and the user might not want to suspend processing.
    #if 0
        // force async if we have a plugin UI to avoid
        // race conditions with concurrent UI updates.
        if (editor_ && !async){
            LOG_VERBOSE("'async' can't be 'false' when using the VST editor");
            async = true;
        }
    #endif
        if (async) {
            suspend();
            auto cmdData = CmdData::create<VendorCmdData>(world(), size);
            if (cmdData) {
                cmdData->index = index;
                cmdData->value = value;
                cmdData->opt = opt;
                cmdData->size = size;
                memcpy(cmdData->data, data, size);
                doCmd(cmdData, cmdVendorSpecific, cmdVendorSpecificDone);
            }
        } else {
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
    ParamStringBuffer str;
    auto len = plugin_->getParameterString(index, str);
    int size = string2floatArray(std::string_view{str.data(), len}, buf + 2, maxSize - 2);
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

void VSTPluginDelegate::sendUpdateDisplay() {
    sendMsg("/vst_update", 0, nullptr);
}

void VSTPluginDelegate::sendPluginCrash(){
    sendMsg("/vst_crash", 0, nullptr);
}

void VSTPluginDelegate::sendMsg(const char *cmd, float f) {
    if (owner_){
        SendNodeReply(&owner_->mParent->mNode, owner_->mParentIndex, cmd, 1, &f);
    } else {
        LOG_ERROR("BUG: VSTPluginDelegate::sendMsg");
    }
}

void VSTPluginDelegate::sendMsg(const char *cmd, int n, const float *data) {
    if (owner_) {
        SendNodeReply(&owner_->mParent->mNode, owner_->mParentIndex, cmd, n, data);
    } else {
        LOG_ERROR("BUG: VSTPluginDelegate::sendMsg");
    }
}

template<typename T>
void VSTPluginDelegate::doCmd(T *cmdData, AsyncStageFn stage2,
    AsyncStageFn stage3, AsyncStageFn stage4) {
    // so we don't have to always check the return value of makeCmdData
    if (cmdData) {
        cmdData->owner.reset(this);
        DoAsynchronousCommand(world(),
            0, 0, cmdData, stage2, stage3, stage4, cmdRTfree<T>, 0, 0);
    }
}

#define DEBUG_REFCOUNT 0

void VSTPluginDelegate::addRef() {
    auto count = refcount_.fetch_add(1);
#if DEBUG_REFCOUNT
    LOG_DEBUG("refcount: " << (count + 1) << " (" << this << ")");
#endif
}

void VSTPluginDelegate::release() {
    auto count = refcount_.fetch_sub(1);
#if DEBUG_REFCOUNT
    LOG_DEBUG("refcount: " << (count - 1) << " (" << this << ")");
#endif
    assert(count >= 1);
    if (count == 1) {
        // last reference
        if (plugin_) {
            // close plugin and defer deletion.
            // (doClose() will increment the refcount again)
            doClose();
        } else {
            auto world = world_;
            this->~VSTPluginDelegate();
            RTFree(world, this);
        }
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
    } else {
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

void vst_mode(VSTPlugin *unit, sc_msg_iter *args) {
    LOG_WARNING("VSTPlugin: /mode command is deprecated and will be ignored");
}

void vst_vis(VSTPlugin* unit, sc_msg_iter *args) {
    bool show = args->geti();
    unit->delegate().showEditor(show);
}

void vst_pos(VSTPlugin* unit, sc_msg_iter *args) {
    int x = args->geti();
    int y = args->geti();
    unit->delegate().setEditorPos(x, y);
}

void vst_size(VSTPlugin* unit, sc_msg_iter *args) {
    int w = args->geti();
    int h = args->geti();
    unit->delegate().setEditorSize(w, h);
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
                } else {
                    unit->delegate().setParam(index, args->getf());
                }
            } else {
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
                    } else {
                        unit->delegate().setParam(index + i, args->getf());
                    }
                }
            } else {
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
    } else {
        unit->delegate().sendMsg("/vst_set", -1);
    }
}

// get a number of parameters starting from index (only values)
void vst_getn(VSTPlugin* unit, sc_msg_iter *args) {
    int32 index = -1;
    if (vst_param_index(unit, args, index)) {
        int32 count = args->geti();
        unit->delegate().getParams(index, count);
    } else {
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
            } else {
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
        } else {
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
    } else {
        LOG_WARNING("vst_program_name: expecting string argument!");
    }
}

void vst_program_read(VSTPlugin* unit, sc_msg_iter *args) {
    if (args->nextTag() == 's') {
        const char* name = args->gets(); // file name
        bool async = args->geti();
        unit->delegate().readPreset<false>(name, async);
    } else {
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
    std::vector<PluginDesc::const_ptr> plugins;
    float timeout = data->timeout;
    bool verbose = data->flags & SearchFlags::verbose;
    bool save = data->flags & SearchFlags::save;
    bool parallel = data->flags & SearchFlags::parallel;
    std::vector<std::string> searchPaths;
    for (int i = 0; i < data->numSearchPaths; ++i){
        searchPaths.push_back(data->pathList[i]);
    }
    std::vector<std::string> excludePaths;
    for (int i = 0; i < data->numExcludePaths; ++i){
        const char *path = data->pathList[data->numSearchPaths + i];
        excludePaths.push_back(normalizePath(path)); // normalize!
    }
    // use default search paths?
    if (searchPaths.empty()) {
        for (auto& path : getDefaultSearchPaths()) {
            // only search if the path actually exists
            if (pathExists(path)){
                searchPaths.push_back(path);
            }
        }
    }
    // search for plugins
    for (auto& path : searchPaths) {
        if (gSearching){
            auto result = searchPlugins(path, excludePaths, timeout, parallel,
                                        (verbose && getVerbosity() >= 0));
            plugins.insert(plugins.end(), result.begin(), result.end());
        } else {
            save = false; // don't update cache file
            LOG_DEBUG("search cancelled");
            break;
        }
    }
#if WARN_VST3_PARAMETERS
    // gWarnPlugins is filled in searchPlugins()
    if (!gWarnPlugins.empty()) {
        Print("\n");
        Print("WARNING: The following VST3 plugins have (non-automatable) parameters which have been omitted "
              "in previous vstplugin~ versions. As a consequence, parameter indices might have changed!\n");
        Print("---\n");
        for (auto& plugin : gWarnPlugins) {
            Print("%s (%s)\n", plugin->key().c_str(), plugin->vendor.c_str());
        }
        Print("\n");
        gWarnPlugins.clear();
    }
#endif
    if (save){
        if (data->cacheFileDir[0]) {
            // use custom cache file directory
            writeCacheFile(data->cacheFileDir);
        } else {
            // use default cache file directory
            writeCacheFile();
        }
    }
#if 1
    // filter duplicate/stale plugins
    plugins.erase(std::remove_if(plugins.begin(), plugins.end(), [](auto& p){
        return getPluginDict().findPlugin(p->key()) != p;
    }), plugins.end());
#endif
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
        } else {
            LOG_ERROR("couldn't write plugin info file '" << data->path << "'!");
        }
    } else if (data->bufnum >= 0) {
        // write to buffer
        auto buf = World_GetNRTBuf(inWorld, data->bufnum);
        // free old buffer data in stage 4.
        // usually, the buffer should be already empty.
        data->freeData = buf->data;
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
    auto n = data->numSearchPaths + data->numExcludePaths;
    for (int i = 0; i < n; ++i){
        RTFree(inWorld, data->pathList[i]);
    }
    return true;
}

void vst_search(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
    setVerbosity(inWorld->mVerbosity);

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
    // timeout
    float timeout = args->getf();
    // collect optional search and exclude paths
    const int maxNumPaths = 256;
    char *pathList[maxNumPaths];
    int numPaths = 0;

    auto collectPaths = [&](){
        int count = 0;
        int n = std::min<int>(args->geti(), maxNumPaths - numPaths);
        for (int i = 0; i < n; ++i) {
            auto s = args->gets();
            if (s) {
                auto len = strlen(s) + 1;
                auto path = (char *)RTAlloc(inWorld, len);
                if (path){
                    memcpy(path, s, len);
                    pathList[numPaths++] = path;
                    count++;
                } else {
                    LOG_ERROR("RTAlloc failed!");
                    break;
                }
            } else {
                LOG_ERROR("wrong number of paths!");
                break;
            }
        }
        return count;
    };

    int numSearchPaths = collectPaths();
    LOG_DEBUG("search paths: " << numSearchPaths);

    int numExcludePaths = collectPaths();
    LOG_DEBUG("exclude paths: " << numExcludePaths);

    assert(numPaths <= maxNumPaths);

    const char *cacheFileDir = args->gets();

    SearchCmdData *data = CmdData::create<SearchCmdData>(inWorld, numPaths * sizeof(char *));
    if (data) {
        data->flags = flags;
        data->timeout = timeout;
        data->bufnum = bufnum; // negative bufnum: don't write search result
        if (filename) {
            snprintf(data->path, sizeof(data->path), "%s", filename);
        } else {
            data->path[0] = '\0'; // empty path: use buffer
        }
        if (cacheFileDir) {
            snprintf(data->cacheFileDir, sizeof(data->cacheFileDir), "%s", cacheFileDir);
        } else {
            data->cacheFileDir[0] = '\0';
        }
        data->numSearchPaths = numSearchPaths;
        data->numExcludePaths = numExcludePaths;
        memcpy(data->pathList, pathList, numPaths * sizeof(char *));
        // LOG_DEBUG("start search");
        gSearching = true; // before command dispatching! -> NRT mode
        DoAsynchronousCommand(inWorld, replyAddr, "vst_search", data,
            cmdSearch, cmdSearchDone, SearchCmdData::nrtFree, RTFree, 0, 0);
    } else {
        for (int i = 0; i < numPaths; ++i){
            RTFree(inWorld, pathList[i]);
        }
    }
}

void vst_search_stop(World* inWorld, void* inUserData, struct sc_msg_iter*args, void* replyAddr) {
    gSearching = false;
}

void vst_clear(World* inWorld, void* inUserData, struct sc_msg_iter* args, void* replyAddr) {
    if (gSearching) {
        LOG_WARNING("can't clear while searching!");
        return;
    }

    struct ClearCmdData { int flags; };

    auto data = (ClearCmdData *)RTAlloc(inWorld, sizeof(ClearCmdData));
    if (data) {
        data->flags = args->geti(); // 1 = remove cache file
        DoAsynchronousCommand(inWorld, replyAddr, "vst_clear", data, [](World*, void* data) {
            // unloading plugins might crash, so we make sure we *first* delete the cache file
            int flags = static_cast<ClearCmdData *>(data)->flags;
            if (flags & 1) {
                // remove cache file
                removeFile(gSettingsDir + "/" + gCacheFileName);
            }
            getPluginDict().clear();
            return false;
        }, 0, 0, RTFree, 0, 0);
    }
}

void vst_cache_read(World *inWorld, void *inUserData, struct sc_msg_iter *args, void *replyAddr) {
    if (gSearching) {
        LOG_WARNING("can't read cache file while searching!");
        return;
    }

    struct CacheReadCmdData { char path[1024]; };

    auto data = (CacheReadCmdData *)RTAlloc(inWorld, sizeof(CacheReadCmdData));
    if (data) {
        auto path = args->gets();
        if (path) {
            snprintf(data->path, sizeof(data->path), "%s", path);
        } else {
            data->path[0] = '\0';
        }
        DoAsynchronousCommand(inWorld, replyAddr, "vst_cache_read", data, [](World*, void* data) {
            std::string dir = static_cast<CacheReadCmdData *>(data)->path;
            if (dir.empty()) {
                dir = gSettingsDir;
            }
            readCacheFile(dir, true);
            return false;
        }, 0, 0, RTFree, 0, 0);
    }
}

// query plugin info
bool cmdQuery(World *inWorld, void *cmdData) {
    auto data = (SearchCmdData *)cmdData;
    auto desc = queryPlugin(data->pathBuf);
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
        } else if (data->bufnum >= 0) {
            // write to buffer
            auto buf = World_GetNRTBuf(inWorld, data->bufnum);
            // free old buffer data in stage 4.
            // usually, the buffer should be already empty.
            data->freeData = buf->data;
            std::stringstream ss;
            LOG_DEBUG("writing plugin info to buffer");
            serializePlugin(ss, *desc);
            allocReadBuffer(buf, ss.str());
        }
        // else do nothing
    }
    return true;
}

bool cmdQueryDone(World* inWorld, void* cmdData) {
    auto data = (SearchCmdData*)cmdData;
    if (data->bufnum >= 0)
        syncBuffer(inWorld, data->bufnum);
    // LOG_DEBUG("query done!");
    return true;
}

void vst_query(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
    setVerbosity(inWorld->mVerbosity);

    if (gSearching) {
        LOG_WARNING("currently searching!");
        return;
    }
    if (args->nextTag() != 's') {
        LOG_ERROR("vst_query: first argument must be a string (plugin path/key)!");
        return;
    }
    int32 bufnum = -1;
    const char* filename = nullptr;
    auto path = args->gets(); // plugin path/key
    auto size = strlen(path) + 1;
    LOG_DEBUG("VSTPlugin: query " << path);
    // temp file or buffer to store the plugin info
    if (args->nextTag() == 's') {
        filename = args->gets();
    } else {
        bufnum = args->geti();
        // negative bufnum allowed (= don't write result)!
        if (bufnum >= (int)inWorld->mNumSndBufs) {
            LOG_ERROR("vst_query: bufnum " << bufnum << " out of range");
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

        memcpy(data->pathBuf, path, size); // store plugin path/key

        DoAsynchronousCommand(inWorld, replyAddr, "vst_query",
            data, cmdQuery, cmdQueryDone, SearchCmdData::nrtFree, RTFree, 0, 0);
    }
}

void vst_dsp_threads(World *inWorld, void* inUserData, struct sc_msg_iter *args, void *replyAddr) {
    int numThreads = args->geti();
    setNumDSPThreads(numThreads);
}

/*** plugin entry point ***/

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

// NOTE: since SC 3.11, Unit commands are queued in the Server, so our hack is not necessary anymore.
// Unfortunately, there is no way to check the SC version at runtime. The next time the plugin API
// version is bumped, we can eventually get rid of it!
template<VSTUnitCmdFunc fn>
void runUnitCmd(VSTPlugin* unit, sc_msg_iter* args) {
#ifdef SUPERNOVA
    // The VSTPlugin constructor might actually run on a DSP helper thread, so we have to
    // make sure that we also mark the main audio thread. Doing this here is the safest option.
    setCurrentThreadRT();
#endif
    if (unit->initialized()) {
        // the constructor has been called, so we can savely run the command
        if (unit->valid()){
            fn(unit, args);
        }
    } else {
        unit->queueUnitCmd((UnitCmdFunc)fn, args); // queue it
    }
}

#define UnitCmd(x) DefineUnitCmd("VSTPlugin", "/" #x, (UnitCmdFunc)runUnitCmd<vst_##x>)
#define PluginCmd(x) DefinePlugInCmd("/" #x, x, 0)

PluginLoad(VSTPlugin) {
    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable

    registerUnit<VSTPlugin>(inTable, "VSTPlugin", true);

    UnitCmd(open);
    UnitCmd(close);
    UnitCmd(reset);
    UnitCmd(mode);

    UnitCmd(vis);
    UnitCmd(pos);
    UnitCmd(size);

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
    PluginCmd(vst_cache_read);
    PluginCmd(vst_clear);
    PluginCmd(vst_query);

    PluginCmd(vst_dsp_threads);

    setLogFunction(SCLog);

    LOG_VERBOSE("VSTPlugin " << getVersionString());

#if 0
    // only read cache file when needed
    readCacheFile();
#endif
}

// NOTE: at the time of writing (SC 3.13), the 'unload' function is not
// documented in the official plugin API (yet), but it is already called
// by scsynth and Supernova!
C_LINKAGE SC_API_EXPORT void unload() {
    // This makes sure that all plugin factories are released here and not
    // in the global object destructor (which can cause crashes or deadlocks!)
    gPluginDict.clear();
}


