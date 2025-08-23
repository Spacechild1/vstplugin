#include "vstplugin~.h"

#include <cassert>
#include <climits>
#include <utility>
#ifdef _WIN32
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#else // _WIN32
# include <dlfcn.h>
#endif

t_signal_setmultiout g_signal_setmultiout;

/*===================== event loop =========================*/

#if POLL_EVENT_LOOP
#define EVENT_LOOP_POLL_INTERVAL 5 // time between polls in ms

static t_clock *eventLoopClock = nullptr;
static void eventLoopTick(void *x){
    UIThread::poll();
    clock_delay(eventLoopClock, EVENT_LOOP_POLL_INTERVAL);
}

static void initEventLoop(){
    static bool done = false;
    if (!done){
        UIThread::setup();

        // start polling if called from main thread
        if (UIThread::isCurrentThread()){
            post("warning: the VST GUI currently runs on the audio thread! "
                 "See the README for more information.");

            eventLoopClock = clock_new(0, (t_method)eventLoopTick);
            clock_delay(eventLoopClock, 0);
        }

        done = true;
    }
}
#else
static void initEventLoop() {}
#endif


/*============================ t_workqueue ============================*/

// Each PDINSTANCE needs its own work queue. Since we don't know when a
// Pd instance is freed, we use reference counting to avoid resource leaks.
#ifdef PDINSTANCE

t_class *workqueue_class;

void t_workqueue::init() {
    auto s = gensym(t_workqueue::bindname);
    auto w = (t_workqueue *)pd_findbyclass(s, workqueue_class);
    if (w) {
        w->w_refcount++;
    } else {
        w = new t_workqueue; // already has refcount of 1
        pd_bind(&w->w_pd, s);
    }
}

void t_workqueue::release() {
    auto s = gensym(t_workqueue::bindname);
    auto w = (t_workqueue *)pd_findbyclass(s, workqueue_class);
    assert(w != nullptr);
    if (--w->w_refcount == 0) {
        pd_unbind(&w->w_pd, s);
        delete w;
    }
}

t_workqueue* t_workqueue::get() {
    auto s = gensym(t_workqueue::bindname);
    auto w = (t_workqueue *)pd_findbyclass(s, workqueue_class);
    assert(w != nullptr);
    return w;
}
#else
// use plain pointer - instead of unique_ptr - so that we can just leak
// the object in case class_free() is not called.
// For example, you can't synchronize threads in a global/static object
// destructor in a Windows DLL because of the loader lock!
// See https://docs.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices
static t_workqueue *gWorkQueue;

void t_workqueue::init() {
    if (!gWorkQueue) {
        gWorkQueue = new t_workqueue;
    }
}

void t_workqueue::release() {}

t_workqueue* t_workqueue::get() {
    return gWorkQueue;
}
#endif

static thread_local bool gWorkerThread{false};

bool isWorkerThread() { return gWorkerThread; }

t_workqueue::t_workqueue(){
#ifdef PDINSTANCE
    w_pd = workqueue_class;
    w_instance = pd_this;
#endif

    w_thread = std::thread([this]{
        LOG_DEBUG("worker thread started");

        vst::setThreadPriority(Priority::Low);

        gWorkerThread = true; // mark as worker thread

    #ifdef PDINSTANCE
        pd_setinstance(w_instance);
    #endif
        w_running.store(true);

        while (w_running.load()) {
            w_event.wait();

            std::unique_lock lock(w_mutex); // for cancellation
            t_item item;
            while (w_nrt_queue.pop(item)){
                if (item.workfn){
                    item.workfn(item.data);
                }
                w_rt_queue.push(item);
                // temporarily release lock in case someone is blocking on cancel()!
                lock.unlock();
                lock.lock();
            }
        }
        LOG_DEBUG("worker thread finished");
    });

    w_clock = clock_new(this, (t_method)clockmethod);
    clock_setunit(w_clock, 1, 1); // use samples
    clock_delay(w_clock, 0);
}

t_workqueue::~t_workqueue() {
    w_running.store(false);
    // wake up and join thread
    w_event.set();
    if (w_thread.joinable()){
        w_thread.join();
    }
    LOG_DEBUG("worker thread joined");
    // The workqueue is either leaked or destroyed in the class free function,
    // so we can safely free the clock! With PDINSTANCE this is even less of a
    // problem because the workqueue is reference counted.
    clock_free(w_clock);
}

void t_workqueue::clockmethod(t_workqueue *w){
    w->poll();
    clock_delay(w->w_clock, 64); // once per DSP tick
}

void t_workqueue::dopush(void *owner, void *data, t_fun<void> workfn,
                        t_fun<void> cb, t_fun<void> cleanup){
    t_item item;
    item.owner = owner;
    item.data = data;
    item.workfn = workfn;
    item.cb = cb;
    item.cleanup = cleanup;
    w_nrt_queue.push(item);
    w_event.set();
}

// cancel all running commands belonging to owner
void t_workqueue::cancel(void *owner){
    std::lock_guard lock(w_mutex);
    // NRT queue
    w_nrt_queue.forEach([&](t_item& i){
        if (i.owner == owner) {
            i.workfn = nullptr;
            i.cb = nullptr;
        }
    });
    // RT queue
    w_rt_queue.forEach([&](t_item& i){
        if (i.owner == owner) {
            i.cb = nullptr;
        }
    });
}

// asynchronous logging to avoid possible deadlocks with sys_lock()/sys_unlock()
void t_workqueue::log(PdLogLevel level, std::string msg) {
    w_log_queue.emplace(level, std::move(msg));
}

void t_workqueue::poll(){
    // poll log messages
    t_logmsg msg;
    while (w_log_queue.pop(msg)) {
        logpost(nullptr, msg.level, "%s", msg.msg.c_str());
    }
    // poll finished tasks
    t_item item;
    while (w_rt_queue.pop(item)){
        if (item.cb){
            item.cb(item.data);
        }
        if (item.cleanup){
            item.cleanup(item.data);
        }
    }
}

void workqueue_terminate() {
    // NOTE: with PDINSTANCE the workqueues are reference counted!
#ifndef PDINSTANCE
    delete gWorkQueue;
#endif
}

void workqueue_setup() {
#ifdef PDINSTANCE
    workqueue_class = class_new(gensym("vstplugin workqueue"),
                                0, 0, 0, CLASS_PD, A_NULL);

#endif
}


/*============================ utility ============================*/

// substitute SPACE for NO-BREAK SPACE (e.g. to avoid Tcl errors in the properties dialog)
static void substitute_whitespace(char *buf){
    for (char *c = buf; *c; c++){
        if (*c == ' ') *c = (char)160;
    }
}

// replace whitespace with underscores so you can type it in Pd
static void bash_name(char *buf){
    for (char *c = buf; *c; c++){
        if (*c == ' ') *c = '_';
    }
}

static void bash_name(std::string& s){
    bash_name(&s[0]);
}

template<typename T>
static bool fromHex(const std::string& s, T& u){
    try {
        u = std::stoull(s, nullptr, 0);
        return true;
    } catch (...){
        return false;
    }
}

template<typename T>
static std::string toHex(T u){
    char buf[MAXPDSTRING];
    snprintf(buf, MAXPDSTRING, "0x%x", (unsigned int)u);
    return buf;
}

template<typename T>
void defer(const T& fn, bool uithread){
    // call NRT method on the correct thread
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


/*============================ logging ============================*/

template<bool async>
class PdLog {
public:
    PdLog(PdLogLevel level = PdNormal)
        : level_(level){}
    PdLog(PdLog&& other) noexcept
        : ss_(std::move(other.ss_)), level_(other.level_) {}
    ~PdLog(){
        auto str = ss_.str();
        if (!str.empty()){
            if (async) {
                // don't use sys_lock()/sys_unlock() to avoid possible deadlocks,
                // e.g. when joining
                t_workqueue::get()->log(level_, std::move(str));
            } else {
                logpost(nullptr, level_, "%s", str.c_str());
            }
        }
    }
    template<typename T>
    PdLog& operator <<(T&& value){
        ss_ << std::forward<T>(value);
        return *this;
    }
    PdLog& operator <<(const ProbeResult& result){
        auto code = result.error.code();
        if (code == Error::NoError){
            *this << "ok!";
        } else {
            // promote to error message
            level_ = PdError;

            switch (result.error.code()){
            case Error::Crash:
                *this << "crashed!";
                break;
            case Error::SystemError:
                *this << "error! " << result.error.what();
                break;
            case Error::ModuleError:
                *this << "couldn't load! " << result.error.what();
                break;
            case Error::PluginError:
                *this << "failed! " << result.error.what();
                break;
            default:
                *this << "unexpected error! " << result.error.what();
                break;
            }
        }
        return *this;
    }
private:
    std::stringstream ss_;
    PdLogLevel level_;
};


/*============================ search/probe ============================*/

static PluginDictionary gPluginDict;

#ifdef PDINSTANCE
SharedMutex gPresetMutex;
#endif

static std::string gSettingsDir = userSettingsPath() + "/pd";

static std::string gCacheFileName = std::string("cache_")
        + cpuArchToString(getHostCpuArchitecture()) + ".ini";

static Mutex gFileLock;

static void readCacheFile(const std::string& dir, bool loud){
    std::lock_guard lock(gFileLock);
    auto path = dir + "/" + gCacheFileName;
    if (pathExists(path)){
        logpost(nullptr, PdDebug, "read cache file %s", path.c_str());
        try {
            gPluginDict.read(path);
        } catch (const Error& e){
            pd_error(nullptr, "couldn't read cache file: %s", e.what());
        } catch (const std::exception& e){
            pd_error(nullptr, "couldn't read cache file: unexpected exception (%s)", e.what());
        }
    } else if (loud) {
        pd_error(nullptr, "could not find cache file in %s", dir.c_str());
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
        pd_error(nullptr, "couldn't write cache file: %s", e.what());
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
        pd_error(nullptr, "couldn't write cache file: %s", e.what());
    }
}

// load factory and probe plugins
template<bool async>
static IFactory::ptr loadFactory(const std::string& path){
    IFactory::ptr factory;

    if (gPluginDict.findFactory(path)){
        PdLog<async>(PdError) << "bug: couldn't find factory '" << path << "'";
        return nullptr;
    }
    if (gPluginDict.isException(path)){
        PdLog<async>(PdDebug) << "'" << path << "' is black-listed";
        return nullptr;
    }

    try {
        factory = IFactory::load(path);
    } catch (const Error& e){
        PdLog<async>(PdError) << "couldn't load '" << path << "': " << e.what();
        gPluginDict.addException(path);
        return nullptr;
    }

    return factory;
}

static void addFactory(const std::string& path, IFactory::ptr factory){
    // LOG_DEBUG("add factory: " << path << " (" << cpuArchToString(factory->arch()) << ")");
    if (factory->numPlugins() == 1){
        auto plugin = factory->getPlugin(0);
        // factories with a single plugin can also be aliased by their file path(s)
        gPluginDict.addPlugin(plugin->path(), plugin);
        gPluginDict.addPlugin(path, plugin);
    }
    gPluginDict.addFactory(path, factory);
    // add plugins
    for (int i = 0; i < factory->numPlugins(); ++i){
        auto plugin = factory->getPlugin(i);
        // also map bashed parameter names
        int num = plugin->parameters.size();
        for (int j = 0; j < num; ++j){
            auto key = plugin->parameters[j].name;
            bash_name(key);
            const_cast<PluginDesc&>(*plugin).addParamAlias(j, key);
        }
        // search for presets
        const_cast<PluginDesc&>(*plugin).scanPresets();
        // add plugin
        auto key = plugin->key();
        gPluginDict.addPlugin(key, plugin);
        bash_name(key); // also add bashed version!
        gPluginDict.addPlugin(key, plugin);
    }
}

template<bool async>
static void postProbeResult(const std::string& path, const ProbeResult& result){
    if (result.total > 1){
        if (result.index == 0){
            PdLog<async>() << "probing '" << path << "' ... ";
        }
        // Pd's posting methods have a size limit, so we log each plugin seperately!
        PdLog<async> log;
        log << "\t[" << (result.index + 1) << "/" << result.total << "] ";
        if (result.plugin && !result.plugin->name.empty()){
            log << "'" << result.plugin->name << "' ";
        }
        log << " ... " << result;
    } else {
        PdLog<async>() << "probing '" << path << "' ... " << result;
    }
}

template<bool async>
static IFactory::ptr probePlugin(const std::string& path, float timeout){
    auto factory = loadFactory<async>(path);
    if (!factory){
        return nullptr;
    }

    try {
        factory->probe([&](const ProbeResult& result){
            postProbeResult<async>(path, result);
        }, timeout);
        if (factory->valid()){
            addFactory(path, factory);
            return factory; // success
        }
    } catch (const Error& e){
        ProbeResult result;
        result.error = e;
        postProbeResult<async>(path, result);
    }
    gPluginDict.addException(path);
    return nullptr;
}

using FactoryFutureResult = std::pair<bool, IFactory::ptr>;
using FactoryFuture = std::function<FactoryFutureResult()>;

template<bool async>
static FactoryFuture probePluginAsync(const std::string& path, float timeout){
    auto factory = loadFactory<async>(path);
    if (!factory) {
        return []() -> FactoryFutureResult {
            return { true, nullptr };
        };
    }
    try {
        // start probing process
        auto future = factory->probeAsync(timeout, true);
        // return future
        return [=]() -> FactoryFutureResult {
            // wait for results
            bool done = future([&](const ProbeResult& result){
                postProbeResult<async>(path, result);
            }); // collect result(s)

            if (done) {
                if (factory->valid()){
                    addFactory(path, factory);
                    return { true, factory }; // success
                } else {
                    gPluginDict.addException(path);
                    return { true, nullptr };
                }
            } else {
                return { false, nullptr }; // not ready
            }
        };
    } catch (const Error& e){
        // return future which prints the error message
        return [=]() -> FactoryFutureResult {
            ProbeResult result;
            result.error = e;
            postProbeResult<async>(path, result);
            gPluginDict.addException(path);
            return { true, nullptr };
        };
    }
}

std::vector<t_symbol *> makePluginList(const std::vector<PluginDesc::const_ptr>& plugins){
    std::vector<t_symbol *> result;
    // convert plugin names to symbols
    for (auto& plugin : plugins){
        auto key = plugin->key();
        bash_name(key);
        result.push_back(gensym(key.c_str()));
    }
    // sort plugin symbols alphabetically and case independent
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs){
        return vst::stringCompare(lhs->s_name, rhs->s_name);
    });
    // remove duplicates
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

#define PROBE_FUTURES 8

template<bool async>
static void searchPlugins(const std::string& path, t_search_data *data){
    bool parallel = data ? data->parallel : true;
    float timeout = data ? data->timeout : 0.f;
    int count = 0;

    std::string bashPath = path;
    sys_unbashfilename(&bashPath[0], &bashPath[0]);
    PdLog<async>() << "searching in '" << bashPath << "' ...";

    auto addPlugin = [&](PluginDesc::const_ptr plugin){
        if (data){
            data->plugins.push_back(plugin);
        }
        count++;
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
                            if (data && plugin->warnParameters) {
                                data->warn_plugins.push_back(plugin);
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
                        PdLog<async>() << "waiting for '" << x.second << "'...";
                    }
                    // PdLog<async> log(PD_NORMAL, "...");
                    last = now;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    vst::search(path, [&](const std::string& absPath){
        if (data && data->cancel){
            return; // cancel search
        }
        LOG_DEBUG("found " << absPath);
        std::string pluginPath = absPath;
        sys_unbashfilename(&pluginPath[0], &pluginPath[0]);
        // check if module has already been loaded
        if (auto factory = gPluginDict.findFactory(pluginPath)) {
            // just post paths of valid plugins
            PdLog<async>(PdDebug) << factory->path();

            auto numPlugins = factory->numPlugins();
            if (numPlugins == 1){
                addPlugin(factory->getPlugin(0));
            } else {
                // add and post plugins
                for (int i = 0; i < numPlugins; ++i){
                    auto plugin = factory->getPlugin(i);
                    addPlugin(plugin);
                    PdLog<async>(PdDebug) << "\t[" << (i + 1) << "/" << numPlugins << "] " << plugin->name;
                }
            }
            // make sure we have the plugin keys!
            for (int i = 0; i < numPlugins; ++i){
                auto plugin = factory->getPlugin(i);
                auto key = plugin->key();
                gPluginDict.addPlugin(key, plugin);
                bash_name(key); // also add bashed version!
                gPluginDict.addPlugin(key, plugin);
            }
        } else {
            // probe (will post results and add plugins)
            if (parallel){
                futures.emplace_back(probePluginAsync<async>(pluginPath, timeout), pluginPath);
                processFutures(PROBE_FUTURES);
            } else {
                if (auto factory = probePlugin<async>(pluginPath, timeout)) {
                    int numPlugins = factory->numPlugins();
                    for (int i = 0; i < numPlugins; ++i) {
                        auto plugin = factory->getPlugin(i);
                        addPlugin(plugin);
                    #if WARN_VST3_PARAMETERS
                        if (data && plugin->warnParameters) {
                            data->warn_plugins.push_back(plugin);
                        }
                    #endif
                    }
                }
            }
        }
    }, true, data->exclude);

    processFutures(0);

    if (count == 1){
        PdLog<async>() << "found 1 plugin";
    } else {
        PdLog<async>() << "found " << count << " plugins";
    }
}

// query a plugin by its key or file path and probe if necessary.
template<bool async>
static const PluginDesc * queryPlugin(const std::string& path) {
    auto desc = gPluginDict.findPlugin(path);
    if (!desc){
        if (probePlugin<async>(path, 0)){
            desc = gPluginDict.findPlugin(path);
            // findPlugin() fails if the module contains several plugins,
            // which means the path can't be used as a key.
            if (!desc){
                PdLog<async>() << "'" << path << "' contains more than one plugin. "
                               << "Please use the 'search' method and open the desired plugin by its key.";
            }
        }
   }
   return desc.get();
}

// tell whether we've already searched the standard VST directory
// (see '-s' flag for [vstplugin~])
static std::atomic_bool gDidSearch{false};


/*========================= t_vstparam =========================*/

static t_class *vstparam_class;

t_vstparam::t_vstparam(t_vstplugin *x, int index)
    : p_pd(vstparam_class), p_owner(x), p_index(index) {
    char buf[64];
    // slider
    snprintf(buf, sizeof(buf), "%p-hsl-%d", x, index);
    p_slider = gensym(buf);
    // display
    snprintf(buf, sizeof(buf), "%p-d-%d-snd", x, index);
    p_display_snd = gensym(buf);
    snprintf(buf, sizeof(buf), "%p-d-%d-rcv", x, index);
    p_display_rcv = gensym(buf);
    // finally bind
    bind();
}

t_vstparam::~t_vstparam() {
    if (p_pd) {
        unbind(); // only if not moved from!
    }
}

t_vstparam::t_vstparam(t_vstparam&& other) noexcept {
    move(other);
}

t_vstparam& t_vstparam::operator=(t_vstparam&& other) noexcept {
    move(other);
    return *this;
}

void t_vstparam::move(t_vstparam &other) {
    if (p_pd) {
        unbind(); // only if not moved from!
    }
    p_pd = other.p_pd;
    p_owner = other.p_owner;
    p_slider = other.p_slider;
    p_display_rcv = other.p_display_rcv;
    p_display_snd = other.p_display_snd;
    p_index = other.p_index;
    bind();
    // other must be valid!
    assert(other.p_pd != nullptr);
    other.unbind();
    other.p_pd = nullptr; // mark as moved!
}

void t_vstparam::bind() {
    pd_bind(&p_pd, p_slider);
    pd_bind(&p_pd, p_display_snd);
}

void t_vstparam::unbind() {
    pd_unbind(&p_pd, p_slider);
    pd_unbind(&p_pd, p_display_snd);
}

// this will set the slider and implicitly call vstparam_set
void t_vstparam::set(t_floatarg f){
    pd_vmess(p_slider->s_thing, gensym("set"), (char *)"f", f);
}

    // called when moving a slider in the generic GUI
static void vstparam_float(t_vstparam *x, t_floatarg f){
    x->p_owner->set_param(x->p_index, f, true);
}

    // called when entering something in the symbol atom
static void vstparam_symbol(t_vstparam *x, t_symbol *s){
    x->p_owner->set_param(x->p_index, s->s_name, true);
}

static void vstparam_set(t_vstparam *x, t_floatarg f){
    // this method updates the display next to the label. implicitly called by t_vstparam::set
    IPlugin &plugin = *x->p_owner->x_plugin;
    int index = x->p_index;
    ParamStringBuffer buf;
    plugin.getParameterString(index, buf);
    pd_vmess(x->p_display_rcv->s_thing, gensym("set"), (char *)"s", gensym(buf.data()));
}

static void vstparam_setup(){
    vstparam_class = class_new(gensym("__vstparam"), 0, 0, sizeof(t_vstparam), 0, A_NULL);
    class_addfloat(vstparam_class, (t_method)vstparam_float);
    class_addsymbol(vstparam_class, (t_method)vstparam_symbol);
    class_addmethod(vstparam_class, (t_method)vstparam_set, gensym("set"), A_DEFFLOAT, 0);
}


/*============================ t_vsteditor ============================*/

t_vsteditor::t_vsteditor(t_vstplugin& owner, bool gui)
    : e_owner(&owner) {
    e_mainthread = std::this_thread::get_id();
    if (gui){
        pd_vmess(&pd_canvasmaker, gensym("canvas"), (char *)"iiiii", 0, 0, 100, 100, 10);
        e_canvas = (t_canvas *)s__X.s_thing;
        send_vmess(gensym("pop"), "i", 0);
    }
    e_clock = clock_new(this, (t_method)tick);
}

t_vsteditor::~t_vsteditor(){
    if (e_canvas){
        pd_free((t_pd *)e_canvas);
    }
    clock_free(e_clock);
    // prevent memleak with sysex events
    t_event e;
    while (e_events.pop(e)) {
        if (e.type == t_event::Sysex){
            delete[] e.sysex.data;
        }
    }
}

// post outgoing event (thread-safe)
void t_vsteditor::post_event(const t_event& event){
    bool mainthread = std::this_thread::get_id() == e_mainthread;
    // check if the Pd scheduler is locked
    bool locked = mainthread || e_locked.load();
    // prevent event scheduling from within the tick method to avoid
    // deadlocks or memory errors
    if (locked && e_tick) {
        pd_error(e_owner, "%s: recursion detected", classname(e_owner));
        return;
    }
    // the event might come from the GUI thread, worker thread or audio thread,
    // but that's why we use a MPSC queue.
    e_events.push(event);

#ifdef PDINSTANCE
    // always set Pd instance because 'locked' being true does not necessarily mean
    // we are on the main thread!
    pd_setinstance(e_owner->x_pdinstance);
#endif
    if (locked) {
        clock_delay(e_clock, 0);
    } else {
        // Set the clock in the perform routine. This is better for real-time safety
        // and it also prevents a possible deadlock with plugins that use a mutex for
        // synchronization between UI thread and processing thread.
        // This also works for the case where the perform routine is called from a
        // different thread than the one where the object has been constructed.
        e_needclock.store(true);

        // Calling pd_getdspstate() is not really thread-safe...
        if (pd_getdspstate() == 0){
            // Special case: DSP might be off and the event does not come from Pd's
            // scheduler thread, neither directly nor indirectly (-> e_locked):
            // To avoid a deadlock, only lock Pd if the event comes from the UI thread or
            // worker thread, otherwise we post a warning and wait for the user to turn on DSP...
            // Generally, this can happen when we receive a notification from the watchdog
            // thread or when a libpd client calls a [vstplugin~] method from another thread.
            // NOTE: calling vst_gui() is safe in this context
            bool canlock = isWorkerThread() || (vst_gui() && UIThread::isCurrentThread());
            if (canlock) {
                // there is still a possible deadlock if post_event() is called from the UI thread
                // while we are waiting in ~vstplugin(), but this is so unlikely that we don't care...
                sys_lock();
                clock_delay(e_clock, 0);
                sys_unlock();
            } else {
                LOG_WARNING("VSTPlugin: received event from unknown thread; cannot dispatch with DSP turned off");
                // will be dispatched when DSP is turned on
            }
        }
    }
}

// parameter automation notification might come from another thread (VST GUI editor).
void t_vsteditor::parameterAutomated(int index, float value){
    t_event e(t_event::Parameter);
    e.param.index = index;
    e.param.value = value;

    post_event(e);
}

void t_vsteditor::updateDisplay() {
    t_event e(t_event::Display);

    post_event(e);
}

// latency change notification might come from another thread
void t_vsteditor::latencyChanged(int nsamples){
    t_event e(t_event::Latency);
    e.latency = nsamples;

    post_event(e);
}

// plugin crash notification might come from another thread
void t_vsteditor::pluginCrashed(){
    t_event e(t_event::Crash);

    post_event(e);
}

// MIDI and SysEx events might be send from both the audio thread (e.g. arpeggiator) or GUI thread (MIDI controller)
void t_vsteditor::midiEvent(const MidiEvent &event){
    t_event e(t_event::Midi);
    e.midi = event;

    post_event(e);
}

void t_vsteditor::sysexEvent(const SysexEvent &event){
    // deep copy!
    auto data = new char[event.size];
    memcpy(data, event.data, event.size);

    t_event e(t_event::Sysex);
    e.sysex.data = data;
    e.sysex.size = event.size;
    e.sysex.delta = event.delta;

    post_event(e);
}

static void vstplugin_close(t_vstplugin *x);

void t_vsteditor::tick(t_vsteditor *x){
    t_outlet *outlet = x->e_owner->x_messout;

    // check for deferred updates
    if (!x->e_param_bitset.empty()) {
        auto& plugin = x->e_owner->x_plugin;
        auto threaded = x->e_owner->x_threaded;
        auto size = x->e_param_bitset_size;
        auto full_size = size * 2;
        // NB: if threaded, dispatch *previous* param changes
        auto param_change = threaded ? x->e_param_bitset.data() + full_size
                                     : x->e_param_bitset.data();
        auto param_auto = param_change + size;
        for (int i = 0; i < size; ++i) {
            if (param_change[i].any()) {
                auto nparams = plugin->info().numParameters();
                for (int j = 0; j < param_num_bits; ++j) {
                    if (param_change[i].test(j)) {
                        auto index = i * param_num_bits+ j;
                        // NB: we need to check the parameter count! See update()
                        if (index < nparams) {
                            auto automated = param_auto[i].test(j);
                            x->param_changed(index, plugin->getParameter(index), automated);
                        }
                    }
                }
                // clear bitset!
                param_change[i].reset();
                param_auto[i].reset(); // !
            }
        }
        if (threaded) {
            // check *new* parameter changes and request clock if necessary
            auto new_param_change = x->e_param_bitset.data();
            if (std::any_of(new_param_change, new_param_change + size,
                            [](auto& x) { return x.any(); })) {
                x->e_needclock.store(true);
            }
            // finally, swap bitsets
            std::swap_ranges(new_param_change, param_change, param_change);
            // all bits should be zero now!
            assert(std::all_of(new_param_change, new_param_change + full_size,
                               [](auto& x) { return x.none(); }));
        }
    }

    // dispatch events
    x->e_tick = true; // prevent recursion
    t_event e;
    while (x->e_events.pop(e)) {
        switch (e.type){
        case t_event::Latency:
        {
            t_atom a;
            int latency = e.latency;
            if (x->e_owner->x_threaded){
                latency += x->e_owner->x_blocksize;
            }
            SETFLOAT(&a, latency);
            outlet_anything(outlet, gensym("latency"), 1, &a);
            break;
        }
        case t_event::Parameter:
        {
            // update the generic GUI
            x->param_changed(e.param.index, e.param.value);
            // send message
            t_atom msg[2];
            SETFLOAT(&msg[0], e.param.index);
            SETFLOAT(&msg[1], e.param.value);
            outlet_anything(outlet, gensym("param_automated"), 2, msg);
            break;
        }
        case t_event::Display:
        {
            // update the generic GUI, e.g. after a program change (VST3)
            x->update(true);
            // send message
            outlet_anything(outlet, gensym("update"), 0, nullptr);
            break;
        }
        case t_event::Crash:
        {
            auto& name = x->e_owner->x_plugin->info().name;
            pd_error(x->e_owner, "plugin '%s' crashed!", name.c_str());

            // send notification
            outlet_anything(outlet, gensym("crash"), 0, nullptr);

            // automatically close plugin
            vstplugin_close(x->e_owner);

            break;
        }
        case t_event::Midi:
        {
            t_atom msg[3];
            SETFLOAT(&msg[0], (unsigned char)e.midi.data[0]);
            SETFLOAT(&msg[1], (unsigned char)e.midi.data[1]);
            SETFLOAT(&msg[2], (unsigned char)e.midi.data[2]);
            outlet_anything(outlet, gensym("midi"), 3, msg);
            break;
        }
        case t_event::Sysex:
        {
            auto msg = new t_atom[e.sysex.size];
            int n = e.sysex.size;
            for (int i = 0; i < n; ++i){
                SETFLOAT(&msg[i], (unsigned char)e.sysex.data[i]);
            }
            outlet_anything(outlet, gensym("sysex"), n, msg);
            delete[] msg;
            delete[] e.sysex.data; // free sysex data!
            break;
        }
        default:
            bug("t_vsteditor::tick");
        }
    }

    x->e_tick = false;
}

constexpr int font_size = 12;
constexpr int xoffset = 30;
constexpr int yoffset = 20;
constexpr int max_params = 16; // max. number of params per column
constexpr int slider_width = 162; // Pd 0.54+
constexpr int slider_height = 18; // Pd 0.54+
constexpr int slider_label_xoffset = -2;
constexpr int slider_label_yoffset = -10;
constexpr int slider_xmargin = 10; // margin between slider and display
constexpr int slider_ymargin = 10; // vertical margin between sliders
constexpr int row_width = 2 * slider_width; // slider + xmargin + symbol atom + label
constexpr int col_height = 2 * slider_height + slider_xmargin;

void t_vsteditor::setup() {
    e_param_bitset.clear(); // always clear!
    e_param_bitset_size = 0;

    if (!pd_gui()){
        return;
    }

    auto& plugin = e_owner->x_plugin;
    auto& info = plugin->info();
    int nparams = info.numParameters();

    if (e_owner->deferred()) {
        // deferred plugin needs deferred parameter updates
        auto d = std::div(nparams, param_num_bits);
        auto size = d.quot + (d.rem > 0);
        // NB: threaded plugins need double buffering!
        auto real_size = e_owner->x_threaded ? size * 4 : 2 * 2;
        e_param_bitset.clear(); // !
        e_param_bitset.resize(real_size);
        e_param_bitset_size = size;
    }

    // setup generic UI
    send_vmess(gensym("rename"), (char *)"s", gensym(info.name.c_str()));
    send_mess(gensym("clear"));

    e_params.clear();
    e_params.reserve(nparams); // avoid unnecessary moves
    for (int i = 0; i < nparams; ++i){
        e_params.emplace_back(e_owner, i);
    }
    // slider: #X obj ...
    char slider_text[256];
    snprintf(slider_text, sizeof(slider_text),
             "25 43 hsl %d %d 0 1 0 0 snd rcv label %d %d 0 %d -262144 -1 -1 0 1",
             slider_width, slider_height, slider_label_xoffset, slider_label_yoffset, font_size);
    t_binbuf *slider_bb = binbuf_new();
    binbuf_text(slider_bb, slider_text, strlen(slider_text));
    t_atom *slider = binbuf_getvec(slider_bb);
    // display: #X symbolatom ...
    char display_text[256];
    snprintf(display_text, sizeof(display_text), "165 79 %d 0 0 1 label rcv snd", font_size);
    t_binbuf *display_bb = binbuf_new();
    binbuf_text(display_bb, display_text, strlen(display_text));
    t_atom *display = binbuf_getvec(display_bb);

    int ncolumns = nparams / max_params + ((nparams % max_params) != 0);
    if (!ncolumns) ncolumns = 1; // just to prevent division by zero
    int nrows = nparams / ncolumns + ((nparams % ncolumns) != 0);

    for (int i = 0; i < nparams; ++i){
        int col = i / nrows;
        int row = i % nrows;
        int xpos = xoffset + col * row_width;
        int ypos = yoffset + slider_height + row * col_height;
        // create slider
        SETFLOAT(slider, xpos);
        SETFLOAT(slider+1, ypos);
        SETSYMBOL(slider+9, e_params[i].p_slider);
        SETSYMBOL(slider+10, e_params[i].p_slider);
        char param_name[64];
        snprintf(param_name, sizeof(param_name), "%d: %s", i, info.parameters[i].name.c_str());
        substitute_whitespace(param_name);
        SETSYMBOL(slider+11, gensym(param_name));
        send_mess(gensym("obj"), 21, slider);
        // create display
        SETFLOAT(display, xpos + slider_width + slider_xmargin);
        SETFLOAT(display+1, ypos);
        SETSYMBOL(display+6, gensym(info.parameters[i].label.c_str()));
        SETSYMBOL(display+7, e_params[i].p_display_rcv);
        SETSYMBOL(display+8, e_params[i].p_display_snd);
        send_mess(gensym("symbolatom"), 9, display);
    }
    float width = row_width * ncolumns + 2 * xoffset;
    float height = nrows * col_height + 2 * yoffset;
    if (width > 1000) width = 1000;
    send_vmess(gensym("setbounds"), "ffff", 0.f, 0.f, width, height);
    width_ = width;
    height_ = height;
    send_vmess(gensym("vis"), "i", 0);

    update(false);

    binbuf_free(slider_bb);
    binbuf_free(display_bb);
}

void t_vsteditor::update(bool allow_defer) {
    if (pd_gui()) {
        if (allow_defer && !e_param_bitset.empty()) {
            // defer update
            // NB: do not set automated!
            for (int i = 0; i < e_param_bitset_size; ++i) {
                e_param_bitset[i].set();
            }
            // NB: deferred plugins require that DSP is running.
            e_needclock.store(true);
        } else {
            // immediate update
            auto& plugin = e_owner->x_plugin;
            int n = plugin->info().numParameters();
            for (int i = 0; i < n; ++i){
                param_changed(i, plugin->getParameter(i));
            }
        }
    }
}

// automated: true if parameter change comes from the (generic) GUI
void t_vsteditor::param_changed(int index, float value, bool automated){
    if (pd_gui() && index >= 0 && index < (int)e_params.size()){
        e_params[index].set(value);
        if (automated){
            parameterAutomated(index, value);
        }
    }
}

// defer parameter update in generic UI.
// This is necessary because the parameter display is not immediately
// available if processing is deferred (threaded or bridged/sandboxed).
void t_vsteditor::param_changed_deferred(int index, bool automated) {
    if (pd_gui()) {
        // set corresponding bit in bitset
        auto i = (uint64_t)index / param_num_bits;
        auto j = (uint64_t)index % param_num_bits;
        assert(i >= 0 && i < e_param_bitset_size);
        e_param_bitset[i].set(j);
        if (automated) {
            e_param_bitset[e_param_bitset_size + i].set(j);
        }
        // NB: deferred plugins require that DSP is running.
        e_needclock.store(true);
    }
}

void t_vsteditor::flush_queues(){
    if (e_needclock.exchange(false)){
        clock_delay(e_clock, 0);
    }
}

template<bool async, typename T>
void t_vsteditor::defer_safe(const T& fn, bool uithread){
    // call on UI thread if we have the plugin UI!
    if (!async) {
        // NOTE: set 'e_locked' *within* the deferred function
        // to avoid possible race conditions with subsequent
        // events on the UI thread.
        defer([&](){
            e_locked.store(true);
            fn();
            e_locked.store(false);
        }, uithread);
    } else {
        defer(fn, uithread);
    }
}

void t_vsteditor::vis(bool v){
    auto win = window();
    if (win){
        if (v){
            win->open();
        } else {
            win->close();
        }
    } else if (e_canvas) {
        send_vmess(gensym("vis"), "i", (int)v);
    }
}

void t_vsteditor::set_pos(int x, int y){
    auto win = window();
    if (win){
        win->setPos(x, y);
    } else if (e_canvas) {
        send_vmess(gensym("setbounds"), "ffff",
            (float)x, (float)y, (float)x + width_, (float)y + height_);
        send_vmess(gensym("vis"), "i", 0);
        send_vmess(gensym("vis"), "i", 1);
    }
}

void t_vsteditor::set_size(int w, int h){
    w = std::max(w, 100);
    h = std::max(h, 100);
    auto win = window();
    if (win){
        win->setSize(w, h);
    } else if (e_canvas) {
        // LATER get the real canvas position
        int x = 20;
        int y = 20;
        send_vmess(gensym("setbounds"), "ffff",
            (float)x, (float)y, (float)x + w, (float)y + h);
        send_vmess(gensym("vis"), "i", 0);
        send_vmess(gensym("vis"), "i", 1);
        width_ = w;
        height_ = h;
    }
}


/*======================== t_vstplugin ========================*/

/*------------------------- "search" --------------------------*/

template<bool async>
static void vstplugin_search_do(t_search_data *x){
    for (auto& path : x->paths){
        if (!x->cancel){
            searchPlugins<async>(path, x); // async
        } else {
            break;
        }
    }

    if (x->update && !x->cancel){
        // cache file and plugin dictionary are threadsafe
        if (!x->cachefiledir.empty()) {
            // custom location
            writeCacheFile(x->cachefiledir);
        } else {
            // default location
            writeCacheFile();
        }
    } else {
        LOG_DEBUG("search cancelled!");
    }
}

static void vstplugin_search_done(t_search_data *x){
    if (x->cancel){
        return; // !
    }
    x->owner->x_search_data = nullptr; // !
    logpost(x, PdNormal, "search done");

#if WARN_VST3_PARAMETERS
    // warn if VST3 plugins have non-automatable parameters
    if (!x->warn_plugins.empty()) {
        post("");
        logpost(x, PdError, "WARNING: The following VST3 plugins have (non-automatable) parameters which have been "
                "omitted in previous vstplugin~ versions. As a consequence, parameter indices might have changed!");
        post("---");
        for (auto& plugin : x->warn_plugins) {
            logpost(x, PdNormal, "%s (%s)", plugin->key().c_str(), plugin->vendor.c_str());
        }
        post("");
    }
#endif

    for (auto& plugin : makePluginList(x->plugins)){
        t_atom msg;
        SETSYMBOL(&msg, plugin);
        outlet_anything(x->owner->x_messout, gensym("plugin"), 1, &msg);
    }
    outlet_anything(x->owner->x_messout, gensym("search_done"), 0, nullptr);
}

static void vstplugin_search(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    float timeout = 0;
    bool async = false;
    bool parallel = true; // for now, always do a parallel search
    bool update = true; // update cache file
    std::string cachefiledir;
    std::vector<std::string> paths;
    std::vector<std::string> exclude;

    if (x->x_search_data){
        pd_error(x, "%s: already searching!", classname(x));
        return;
    }

    while (argc && argv->a_type == A_SYMBOL){
        auto flag = argv->a_w.w_symbol->s_name;
        if (*flag == '-'){
            if (!strcmp(flag, "-a")){
                async = true;
            } else if (!strcmp(flag, "-n")){
                update = false;
            } else if (!strcmp(flag, "-t")){
                argc--; argv++;
                if (argc > 0 && argv->a_type == A_FLOAT){
                    timeout = argv->a_w.w_float;
                } else {
                    pd_error(x, "%s: missing argument for -t flag", classname(x));
                    return;
                }
            } else if (!strcmp(flag, "-x")){
                argc--; argv++;
                if (argc > 0 && argv->a_type == A_SYMBOL){
                    auto sym = atom_getsymbol(argv);
                    char path[MAXPDSTRING];
                    canvas_makefilename(x->x_canvas, sym->s_name, path, MAXPDSTRING);
                    exclude.push_back(normalizePath(path)); // normalize!
                } else {
                    pd_error(x, "%s: missing argument for -x flag", classname(x));
                    return;
                }
            } else if (!strcmp(flag, "-c")) {
                argc--; argv++;
                if (argc > 0 && argv->a_type == A_SYMBOL){
                    cachefiledir = atom_getsymbol(argv)->s_name;
                } else {
                    pd_error(x, "%s: missing argument for -c flag", classname(x));
                    return;
                }
            } else {
                pd_error(x, "%s: unknown flag '%s'", classname(x), flag);
                return;
            }
            argv++; argc--;
        } else {
            break;
        }
    }

    if (argc > 0){
        while (argc--){
            auto sym = atom_getsymbol(argv++);
            char path[MAXPDSTRING];
            canvas_makefilename(x->x_canvas, sym->s_name, path, MAXPDSTRING);
            paths.emplace_back(path);
        }
    } else {
        // search in the default VST search paths if no user paths were provided
        for (auto& path : getDefaultSearchPaths()){
            // only search if the path actually exists
            if (pathExists(path)){
                paths.emplace_back(path);
            }
        }
    }

    if (async){
        auto data = new t_search_data();
        data->owner = x;
        data->paths = std::move(paths);
        data->exclude = std::move(exclude);
        data->cachefiledir = cachefiledir;
        data->timeout = timeout;
        data->parallel = parallel;
        data->update = update;
        x->x_search_data = data;
        t_workqueue::get()->push(x, data, vstplugin_search_do<true>, vstplugin_search_done);
    } else {
        t_search_data data;
        data.owner = x;
        data.paths = std::move(paths);
        data.exclude = std::move(exclude);
        data.cachefiledir = cachefiledir;
        data.timeout = timeout;
        data.parallel = parallel;
        data.update = update;
        vstplugin_search_do<false>(&data);
        vstplugin_search_done(&data);
    }
}

/*----------------------- "search_stop" -------------------------*/

static void vstplugin_search_stop(t_vstplugin *x){
    if (x->x_search_data){
        x->x_search_data->cancel = true;
        x->x_search_data = nullptr; // will be freed by work queue
    }
}

/*----------------------- "cache_clear" ------------------------*/

static void vstplugin_cache_clear(t_vstplugin *x, t_floatarg f){
    // unloading plugins might crash, so we first delete the cache file
    if (f != 0){
        removeFile(gSettingsDir + "/" + gCacheFileName);
    }
    // clear the plugin description dictionary
    gPluginDict.clear();
}

/*----------------------- "cache_read" ------------------------*/

static void vstplugin_cache_read(t_vstplugin *x, t_symbol *s){
    if (*s->s_name) {
        // custom location
        readCacheFile(s->s_name, true);
    } else {
        // default location
        readCacheFile();
    }
}

/*----------------------- "plugin_list" -------------------------*/

static void vstplugin_plugin_list(t_vstplugin *x){
    auto plugins = gPluginDict.pluginList();
    for (auto& plugin : makePluginList(plugins)){
        t_atom msg;
        SETSYMBOL(&msg, plugin);
        outlet_anything(x->x_messout, gensym("plugin"), 1, &msg);
    }
}

/*-------------------------- "close" ----------------------------*/

struct t_close_data : t_command_data<t_close_data> {
    IPlugin::ptr plugin;
    bool uithread;
};

static void vstplugin_close(t_vstplugin *x){
    if (!x->x_plugin){
        return;
    }
    if (x->x_suspended){
        pd_error(x, "%s: can't close plugin - temporarily suspended!",
                 classname(x));
        return;
    }
#if 0
    x->x_plugin->setListener(nullptr);
#endif
    // make sure to release the plugin on the same thread where it was opened!
    // this is necessary to avoid crashes or deadlocks with certain plugins.
    if (x->x_async){
        // NOTE: if we close the plugin asynchronously and the plugin editor
        // is opened, it can happen that an event is sent from the UI thread,
        // e.g. when automating parameters in the plugin UI.
        // Since those events come from the UI thread, unsetting the listener
        // here in the audio thread would create a race condition.
        // Instead, we unset the listener implicitly when we close the plugin.
        // However, this is dangerous if we close the plugin asynchronously
        // immediately before or inside ~t_vstplugin.
        // We can't sync with the plugin closing on the UI thread, as the
        // actual close request is issued on the NRT thread and can execute
        // *after* ~t_vstplugin. We *could* wait for all pending NRT commands
        // to finish, but that would be overkill. Instead we close the editor
        // *here* and sync with the UI thread in ~t_vstplugin, assuming that
        // the plugin can't send UI events without the editor.
        auto window = x->x_plugin->getWindow();
        if (window){
            window->close(); // see above
        }

        auto data = new t_close_data();
        data->plugin = std::move(x->x_plugin);
        data->uithread = x->x_uithread;
        t_workqueue::get()->push(x, data,
            [](t_close_data *x){
                defer([&](){
                    x->plugin = nullptr;
                }, x->uithread);
            }, nullptr);
    } else {
        defer([&](){
            x->x_plugin = nullptr;
        }, x->x_uithread);
    }

    x->x_plugin = nullptr;
    x->x_process = false;
    x->x_editor->vis(false);
    x->x_key = nullptr;
    x->x_path = nullptr;
    x->x_preset = nullptr;
    x->x_inputs.clear();
    x->x_outputs.clear();
    x->x_input_channels = 0;
    x->x_output_channels = 0;

    // notify
    outlet_anything(x->x_messout, gensym("close"), 0, nullptr);
}

/*-------------------------- "open" ----------------------------*/

struct t_open_data : t_command_data<t_open_data> {
    t_symbol *pathsym;
    std::string abspath;
    IPlugin::ptr plugin;
    bool editor;
    bool threaded;
    RunMode mode;
    std::string errmsg;
};

template<bool async>
static void vstplugin_open_do(t_open_data *data){
    auto x = data->owner;
    // get plugin info
    const PluginDesc *info = queryPlugin<async>(data->abspath);
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
            x->x_editor->defer_safe<async>([&](){
                // create plugin
                data->plugin = info->create(data->editor, data->threaded, data->mode);
                // setup plugin
                // protect against concurrent vstplugin_dsp() and vstplugin_save()
                t_scoped_nrt_lock lock(x->x_mutex);
                x->setup_plugin(*data->plugin);
            }, data->editor);
            LOG_DEBUG("done");
        } catch (const Error & e) {
            // shouldn't happen...
            data->errmsg = e.what();
        }
    }
}

static void vstplugin_open_done(t_open_data *data){
    auto x = data->owner;
    if (data->plugin){
        x->x_plugin = std::move(data->plugin);
        x->x_uithread = data->editor; // remember *where* we opened the plugin
        x->x_threaded = data->threaded;
        x->x_runmode = data->mode;

        auto& info = x->x_plugin->info();

        // warn about processing precision mismatch, see setup_plugin().
        if (x->x_process) {
            if (x->x_wantprecision != x->x_realprecision) {
                if (x->x_wantprecision == ProcessPrecision::Double){
                    logpost(x, PdNormal, "warning: '%s' doesn't support double precision,"
                            " using single precision instead", info.name.c_str());
                } else {
                    logpost(x, PdNormal, "warning: '%s' doesn't support single precision,"
                            " using double precision instead", info.name.c_str());
                }
            }
        } else {
            logpost(x, PdNormal, "warning: '%s' doesn't support single or double precision"
                    " - bypassing", info.name.c_str());
        }

        // after setting the plugin!
        x->update_buffers();

        // do it here instead of vstplugin_open_do() to avoid race condition
        // with "bypass" method
        if (x->x_bypass != Bypass::Off){
            x->x_plugin->setBypass(x->x_bypass);
        }

        // store key (mainly needed for preset change notification)
        x->x_key = gensym(info.key().c_str());
        // store path symbol (to avoid reopening the same plugin)
        x->x_path = data->pathsym;
        // receive events from plugin
        x->x_plugin->setListener(x->x_editor.get());
        // setup Pd editor
        x->x_editor->setup();

        logpost(x, PdDebug, "opened '%s'", info.name.c_str());
    } else {
        if (!data->errmsg.empty()) {
            pd_error(x, "%s", data->errmsg.c_str());
        }
        pd_error(x, "%s: cannot open '%s'", classname(x), data->pathsym->s_name);
    }
}

static void vstplugin_open_notify(t_vstplugin *x) {
    // output message
    bool success = x->x_plugin != nullptr;
    t_atom msg[2];
    SETFLOAT(&msg[0], success);
    if (success){
        // Output the original path, *not* the plugin name!
        // It might be used to query information about
        // the plugin, so we better make sure that it
        // really refers to the same plugin.
        SETSYMBOL(&msg[1], x->x_path);
    }
    outlet_anything(x->x_messout, gensym("open"), (success ? 2 : 1), msg);

    if (success) {
        // report initial latency
        t_atom a;
        int latency = x->x_plugin->getLatencySamples();
        if (x->x_threaded){
            latency += x->x_blocksize;
        }
        SETFLOAT(&a, latency);
        outlet_anything(x->x_messout, gensym("latency"), 1, &a);
    }
}

static void vstplugin_open(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    t_symbol *pathsym = nullptr;
    bool editor = false;
    bool async = false;
    bool threaded = false;
    auto mode = RunMode::Auto;
    // parse arguments
    while (argc && argv->a_type == A_SYMBOL){
        auto sym = argv->a_w.w_symbol;
        if (*sym->s_name == '-'){ // flag
            const char *flag = sym->s_name;
            if (!strcmp(flag, "-e")){
                editor = true;
            } else if (!strcmp(flag, "-t")){
                threaded = true;
            } else if (!strcmp(flag, "-p")){
                mode = RunMode::Sandbox;
            } else if (!strcmp(flag, "-b")){
                mode = RunMode::Bridge;
            } else {
                pd_error(x, "%s: unknown flag '%s'", classname(x), flag);
            }
            argc--; argv++;
        } else { // file name
            pathsym = sym;
            if (--argc){
                // "async" float argument after plugin name
                async = atom_getfloat(++argv);
            }
            break;
        }
    }

    if (!pathsym){
        pd_error(x, "%s: 'open' needs a symbol argument!", classname(x));
        return;
    }
#if 0
    if (!*pathsym->s_name){
        pd_error(x, "%s: empty symbol for 'open' message!", classname(x));
        return;
    }
#endif
    // don't reopen the same plugin (mainly for -k flag), unless options have changed.
    if (pathsym == x->x_path && editor == x->x_uithread
            && threaded == x->x_threaded && mode == x->x_runmode) {
        return;
    }
    // don't open while async command is running
    if (x->x_suspended){
        pd_error(x, "%s: can't open plugin - temporarily suspended!",
                 classname(x));
        return;
    }
    // close the old plugin
    vstplugin_close(x);

    // for editor or plugin bridge/sandbox
    initEventLoop();

    auto abspath = x->resolve_plugin_path(pathsym->s_name);
    if (abspath.empty()) {
        pd_error(x, "%s: cannot open '%s'", classname(x), pathsym->s_name);
        // output failure!
        vstplugin_open_notify(x);
        return;
    }

    // open the new plugin
    if (async){
        auto data = new t_open_data();
        data->owner = x;
        data->pathsym = pathsym;
        data->abspath = abspath;
        data->editor = editor;
        data->threaded = threaded;
        data->mode = mode;
        t_workqueue::get()->push(
            x, data, vstplugin_open_do<true>,
            [](t_open_data *x){
                vstplugin_open_done(x);
                vstplugin_open_notify(x->owner);
            });
    } else {
        t_open_data data;
        data.owner = x;
        data.pathsym = pathsym;
        data.abspath = abspath;
        data.editor = editor;
        data.threaded = threaded;
        data.mode = mode;
        vstplugin_open_do<false>(&data);
        vstplugin_open_done(&data);
        vstplugin_open_notify(x);
    }
    x->x_async = async; // remember *how* we openend the plugin
    // NOTE: don't set 'x_uithread' already because 'editor' value might change
}

/*-------------------------- "info" -------------------------*/

static void sendInfo(t_vstplugin *x, const char *what, std::string_view value){
    t_atom msg[2];
    SETSYMBOL(&msg[0], gensym(what));
    SETSYMBOL(&msg[1], gensym(value.data()));
    outlet_anything(x->x_messout, gensym("info"), 2, msg);
}

static void sendInfo(t_vstplugin *x, const char *what, int value){
    t_atom msg[2];
    SETSYMBOL(&msg[0], gensym(what));
    SETFLOAT(&msg[1], value);
    outlet_anything(x->x_messout, gensym("info"), 2, msg);
}

// plugin info (no args: currently loaded plugin, symbol arg: path of plugin to query)
static void vstplugin_info(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    const PluginDesc *info = nullptr;
    if (argc > 0){ // some plugin
        auto path = atom_getsymbol(argv)->s_name;
        auto abspath = x->resolve_plugin_path(path);
        if (abspath.empty() || !(info = queryPlugin<false>(abspath))){
            pd_error(x, "%s: couldn't open '%s' - no such file or plugin!", classname(x), path);
            return;
        }
    } else { // this plugin
        if (!x->check_plugin()) return;
        info = &x->x_plugin->info();
    }
    sendInfo(x, "path", info->path());
    sendInfo(x, "name", info->name);
    sendInfo(x, "vendor", info->vendor);
    sendInfo(x, "category", info->category);
    sendInfo(x, "version", info->version);
    sendInfo(x, "sdkversion", info->sdkVersion);
    // deprecated
#if 1
    sendInfo(x, "inputs", info->numInputs() > 0 ?
                 info->inputs[0].numChannels : 0);
    sendInfo(x, "outputs", info->numOutputs() > 0 ?
                 info->outputs[0].numChannels : 0);
    sendInfo(x, "auxinputs", info->numInputs() > 1 ?
                 info->inputs[1].numChannels : 0);
    sendInfo(x, "auxoutputs", info->numOutputs() > 1 ?
                 info->outputs[1].numChannels : 0);
#endif
    sendInfo(x, "id", ("0x"+info->uniqueID));
    sendInfo(x, "editor", info->editor());
    sendInfo(x, "resizable", info->editorResizable());
    sendInfo(x, "synth", info->synth());
    sendInfo(x, "single", info->singlePrecision());
    sendInfo(x, "double", info->doublePrecision());
    sendInfo(x, "midiin", info->midiInput());
    sendInfo(x, "midiout", info->midiOutput());
    sendInfo(x, "sysexin", info->sysexInput());
    sendInfo(x, "sysexout", info->sysexOutput());
    sendInfo(x, "bridged", info->bridged());
}

/*-------------------------- "can_do" ----------------------------*/

// query plugin for capabilities
static void vstplugin_can_do(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    int result = x->x_plugin->canDo(s->s_name);
    t_atom msg[2];
    SETSYMBOL(&msg[0], s);
    SETFLOAT(&msg[1], result);
    outlet_anything(x->x_messout, gensym("can_do"), 2, msg);
}

/*----------------------- "vendor_method" ------------------------*/

// vendor specific action (index, value, opt, data)
static void vstplugin_vendor_method(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    int index = 0;
    intptr_t value = 0;

    // get integer argument as number or hex string
    auto getInt = [&](int which, auto& var){
        if (argc > which){
            if (argv->a_type == A_SYMBOL){
                auto c = argv->a_w.w_symbol->s_name;
                if (!fromHex(c, var)){
                    pd_error(x, "%s: couldn't convert '%s'", classname(x), c);
                    return false;
                }
            } else {
                var = atom_getfloat(argv);
            }
        }
        return true;
    };

    if (!getInt(0, index)) return;
    if (!getInt(1, value)) return;
    float opt = atom_getfloatarg(2, argc, argv);
    int size = argc - 3;
    std::unique_ptr<char[]> data;
    if (size > 0){
        data = std::make_unique<char[]>(size);
        for (int i = 0, j = 3; i < size; ++i, ++j){
            data[i] = atom_getfloat(argv + j);
        }
    }

    intptr_t result;
    x->x_editor->defer_safe<false>([&](){
        result = x->x_plugin->vendorSpecific(index, value, data.get(), opt);
    }, x->x_uithread);

    t_atom msg[2];
    SETFLOAT(&msg[0], result);
    SETSYMBOL(&msg[1], gensym(toHex(result).c_str()));
    outlet_anything(x->x_messout, gensym("vendor_method"), 2, msg);
}

/*-------------------------- "print" -----------------------------*/

// print plugin info in Pd console
static void vstplugin_print(t_vstplugin *x){
    if (!x->check_plugin()) return;
    auto& info = x->x_plugin->info();
    bool vst3 = info.type() == PluginType::VST3;

    post("---");
    post("name: %s", info.name.c_str());
    post("type: %s%s%s", info.sdkVersion.c_str(),
         info.synth() ? " (synth)" : "",
         info.bridged() ? " [bridged] " : "");
    post("version: %s", info.version.c_str());
    post("path: %s", info.path().c_str());
    post("vendor: %s", info.vendor.c_str());
    post("category: %s", info.category.c_str());

    auto postBusses = [](auto& busses, auto what, auto vst3){
        if (busses.size() > 0){
            if (vst3){
                post("%s:", what);
                for (auto& bus : busses){
                    auto type = (bus.type == PluginDesc::Bus::Aux) ?
                                "aux" : "main";
                    post("  [%s] '%s' %dch", type,
                         bus.label.c_str(), bus.numChannels);
                }
            } else {
                // always a single bus (no additional info)!
                if (busses[0].numChannels > 0){
                    post("%s: %dch", what, busses[0].numChannels);
                } else {
                    post("%s: none", what);
                }
            }
        } else {
            post("%s: none", what);
        }
    };
    postBusses(info.inputs, "inputs", vst3);
    postBusses(info.outputs, "outputs", vst3);

    post("parameters: %d", info.numParameters());
    post("programs: %d", info.numPrograms());
    post("presets: %d", info.numPresets());
    post("editor: %s", info.editor() ? "yes" : "no");
    post("resizable: %s", info.editorResizable() ? "yes" : "no");
    post("single precision: %s", info.singlePrecision() ? "yes" : "no");
    post("double precision: %s", info.doublePrecision() ? "yes" : "no");
    post("midi input: %s", info.midiInput() ? "yes" : "no");
    post("midi output: %s", info.midiOutput() ? "yes" : "no");
    post("---");
}

/*-------------------------- "version" ---------------------------*/

static void vstplugin_version(t_vstplugin *x){
    t_atom msg[3];
    SETFLOAT(&msg[0], VERSION_MAJOR);
    SETFLOAT(&msg[1], VERSION_MINOR);
    SETFLOAT(&msg[2], VERSION_PATCH);

    outlet_anything(x->x_messout, gensym("version"), 3, msg);
}

/*-------------------------- "bypass" ----------------------------*/

// bypass the plugin
static void vstplugin_bypass(t_vstplugin *x, t_floatarg f){
    int arg = f;
    Bypass bypass;
    switch (arg){
    case 0:
        bypass = Bypass::Off;
        break;
    case 1:
        bypass = Bypass::Hard;
        break;
    case 2:
        bypass = Bypass::Soft;
        break;
    default:
        pd_error(x, "%s: bad argument for 'bypass'' message (%d)", classname(x), arg);
        return;
    }
    if (x->x_plugin && (bypass != x->x_bypass)){
        x->x_plugin->setBypass(bypass);
    }
    x->x_bypass = bypass;
}

/*-------------------------- "reset" ----------------------------*/

struct t_reset_data : t_command_data<t_reset_data> {};

// reset the plugin state
static void vstplugin_reset(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    bool async = f;
    if (async){
        auto data = new t_reset_data();
        data->owner = x;
        t_workqueue::get()->push(x, data,
            [](t_reset_data *x){
                auto& plugin = x->owner->x_plugin;
                x->owner->x_editor->defer_safe<true>([&](){
                    // protect against vstplugin_dsp() and vstplugin_save()
                    t_scoped_nrt_lock lock(x->owner->x_mutex);
                    plugin->suspend();
                    plugin->resume();
                }, x->owner->x_uithread);
            },
            [](t_reset_data *x){
                x->owner->x_suspended = false;
                outlet_anything(x->owner->x_messout,
                                gensym("reset"), 0, nullptr);
            }
        );
        x->x_suspended = true;
    } else {
        // protect against concurrent reads/writes
        x->x_editor->defer_safe<false>([&](){
            t_scoped_nrt_lock lock(x->x_mutex);
            x->x_plugin->suspend();
            x->x_plugin->resume();
        }, x->x_uithread);
        outlet_anything(x->x_messout, gensym("reset"), 0, nullptr);
    }
}

/*-------------------------- "offline" ----------------------------*/

// enable/disable offline processing
static void vstplugin_offline(t_vstplugin *x, t_floatarg f){
    ProcessMode mode = (f != 0) ? ProcessMode::Offline : ProcessMode::Realtime;

    if (x->x_plugin && (mode != x->x_mode)){
        x->x_editor->defer_safe<false>([&](){
            t_scoped_nrt_lock lock(x->x_mutex);
            x->x_plugin->suspend();
            x->x_plugin->setupProcessing(x->x_sr, x->x_blocksize,
                                         x->x_realprecision, mode);
            x->x_plugin->resume();
        }, x->x_uithread);
    }

    x->x_mode = mode;
}

/*~~~~~~~~~~~~~~~~~~~~ editor window ~~~~~~~~~~~~~~~~~~~~~~~~~~*/

// show/hide editor window
static void vstplugin_vis(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_editor->vis(f);
}
// move the editor window
static void vstplugin_pos(t_vstplugin *x, t_floatarg x_, t_floatarg y_){
    if (!x->check_plugin()) return;
    x->x_editor->set_pos(x_, y_);
}

static void vstplugin_size(t_vstplugin *x, t_floatarg w, t_floatarg h){
    if (!x->check_plugin()) return;
    x->x_editor->set_size(w, h);
}

static void vstplugin_click(t_vstplugin *x){
    vstplugin_vis(x, 1);
}

/*~~~~~~~~~~~~~~~~~~~~~~~ transport ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

// set tempo in BPM
static void vstplugin_tempo(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    if (f > 0){
        x->x_plugin->setTempoBPM(f);
    } else {
        pd_error(x, "%s: tempo must greater than 0", classname(x));
    }
}

// set time signature
static void vstplugin_time_signature(t_vstplugin *x, t_floatarg num, t_floatarg denom){
    if (!x->check_plugin()) return;
    if (num > 0 && denom > 0){
        x->x_plugin->setTimeSignature(num, denom);
    } else {
        pd_error(x, "%s: bad time signature", classname(x));
    }
}

// play/stop
static void vstplugin_play(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_plugin->setTransportPlaying(f);
}

// cycle
static void vstplugin_cycle(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_plugin->setTransportCycleActive(f);
}

static void vstplugin_cycle_start(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_plugin->setTransportCycleStart(f);
}

static void vstplugin_cycle_end(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_plugin->setTransportCycleEnd(f);
}

// set transport position (quarter notes)
static void vstplugin_transport_set(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_plugin->setTransportPosition(f);
}

// get current transport position
static void vstplugin_transport_get(t_vstplugin *x){
    if (!x->check_plugin()) return;
    t_atom a;
    SETFLOAT(&a, x->x_plugin->getTransportPosition());
    outlet_anything(x->x_messout, gensym("transport"), 1, &a);
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~ I/O info ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

enum class t_direction {
    in,
    out
};

// get bus info (index + nchannels + name + type ...)
template<t_direction dir>
static void vstplugin_bus_doinfo(const PluginDesc& info, int index, t_outlet *outlet){
    auto& bus = (dir == t_direction::out) ?
                info.outputs[index] : info.inputs[index];
    auto vst3 = info.type() == PluginType::VST3;
    t_atom msg[4];
    SETFLOAT(&msg[0], index);
    SETFLOAT(&msg[1], bus.numChannels);
    if (vst3){
        SETSYMBOL(&msg[2], gensym(bus.label.c_str()));
        SETSYMBOL(&msg[3], (bus.type == PluginDesc::Bus::Aux) ?
                    gensym("aux") : gensym("main"));
    }
    // LATER add more info
    auto sel = (dir == t_direction::out) ?
                gensym("output_info") : gensym("input_info");
    outlet_anything(outlet, sel, vst3 ? 4 : 2, msg);
}

template<t_direction dir>
static void vstplugin_bus_info(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    int index = f;
    auto& info = x->x_plugin->info();
    if (index >= 0 && index < info.numInputs()){
        vstplugin_bus_doinfo<dir>(info, index, x->x_messout);
    } else {
        auto what = (dir == t_direction::out) ? "output" : "input";
        pd_error(x, "%s: %s bus index %d out of range!",
                 classname(x), what, index);
    }
}

// number of inputs/outputs
static void vstplugin_input_count(t_vstplugin *x){
    if (!x->check_plugin()) return;
    t_atom msg;
    SETFLOAT(&msg, x->x_plugin->info().numInputs());
    outlet_anything(x->x_messout, gensym("input_count"), 1, &msg);
}

static void vstplugin_output_count(t_vstplugin *x){
    if (!x->check_plugin()) return;
    t_atom msg;
    SETFLOAT(&msg, x->x_plugin->info().numOutputs());
    outlet_anything(x->x_messout, gensym("output_count"), 1, &msg);
}

// list busses (index + info)
template<t_direction dir>
static void vstplugin_bus_list(t_vstplugin *x, t_symbol *s){
    const PluginDesc *info = nullptr;
    if (*s->s_name){
        auto abspath = x->resolve_plugin_path(s->s_name);
        if (abspath.empty() || !(info = queryPlugin<false>(abspath))){
            pd_error(x, "%s: couldn't open '%s' - no such file or plugin!",
                     classname(x), s->s_name);
            return;
        }
    } else {
        if (!x->check_plugin()) return;
        info = &x->x_plugin->info();
    }
    int n = (dir == t_direction::out) ?
                info->numOutputs() : info->numInputs();
    for (int i = 0; i < n; ++i){
        vstplugin_bus_doinfo<dir>(*info, i, x->x_messout);
    }
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~ parameters ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

static bool findParamIndex(t_vstplugin *x, t_atom *a, int& index){
    if (a->a_type == A_SYMBOL){
        auto name = a->a_w.w_symbol->s_name;
        index = x->x_plugin->info().findParam(name);
        if (index < 0){
            pd_error(x, "%s: couldn't find parameter '%s'", classname(x), name);
            return false;
        }
    } else {
        index = atom_getfloat(a);
    }
    return true;
}

// set parameter by float (0.0 - 1.0) or string (if supported)
static void vstplugin_param_set(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    if (argc < 2){
        pd_error(x, "%s: 'param_set' expects two arguments (index/name + float/symbol)", classname(x));
        return;
    }
    int index = -1;
    if (!findParamIndex(x, argv, index)) return;
    if (argv[1].a_type == A_SYMBOL)
        x->set_param(index, argv[1].a_w.w_symbol->s_name, false);
    else
        x->set_param(index, atom_getfloat(argv + 1), false);
}

// get parameter state (value + display)
static void vstplugin_param_get(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    if (!argc){
        pd_error(x, "%s: 'param_get' expects index/name argument", classname(x));
        return;
    }
    int index = -1;
    if (!findParamIndex(x, argv, index)) return;
    if (index >= 0 && index < x->x_plugin->info().numParameters()) {
        ParamStringBuffer str;
        x->x_plugin->getParameterString(index, str);

        t_atom msg[3];
        SETFLOAT(&msg[0], index);
        SETFLOAT(&msg[1], x->x_plugin->getParameter(index));
        SETSYMBOL(&msg[2], gensym(str.data()));
        outlet_anything(x->x_messout, gensym("param_state"), 3, msg);
    } else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
    }
}

// get parameter info (name + label + automatable)
static void vstplugin_param_doinfo(const PluginDesc& info, int index, t_outlet *outlet){
    auto& param = info.parameters[index];
    t_atom msg[4];
    SETFLOAT(&msg[0], index);
    SETSYMBOL(&msg[1], gensym(param.name.c_str()));
    SETSYMBOL(&msg[2], gensym(param.label.c_str()));
    SETFLOAT(&msg[3], param.automatable);
    // LATER add more info
    outlet_anything(outlet, gensym("param_info"), 4, msg);
}

// get parameter info (name + label + ...)
static void vstplugin_param_info(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    int index = f;
    auto& info = x->x_plugin->info();
    if (index >= 0 && index < info.numParameters()){
        vstplugin_param_doinfo(info, index, x->x_messout);
    } else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
    }
}

// number of parameters
static void vstplugin_param_count(t_vstplugin *x){
    if (!x->check_plugin()) return;
    t_atom msg;
    SETFLOAT(&msg, x->x_plugin->info().numParameters());
    outlet_anything(x->x_messout, gensym("param_count"), 1, &msg);
}

// list parameters (index + info)
static void vstplugin_param_list(t_vstplugin *x, t_symbol *s){
    const PluginDesc *info = nullptr;
    if (*s->s_name){
        auto abspath = x->resolve_plugin_path(s->s_name);
        if (abspath.empty() || !(info = queryPlugin<false>(abspath))){
            pd_error(x, "%s: couldn't open '%s' - no such file or plugin!",
                     classname(x), s->s_name);
            return;
        }
    } else {
        if (!x->check_plugin()) return;
        info = &x->x_plugin->info();
    }
    int n = info->numParameters();
    for (int i = 0; i < n; ++i){
        vstplugin_param_doinfo(*info, i, x->x_messout);
    }
}

// list parameter states (index + value)
static void vstplugin_param_dump(t_vstplugin *x){
    if (!x->check_plugin()) return;
    int n = x->x_plugin->info().numParameters();
    for (int i = 0; i < n; ++i){
        t_atom a;
        SETFLOAT(&a, i);
        vstplugin_param_get(x, 0, 1, &a);
    }
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~ MIDI ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

// send raw MIDI message
static void vstplugin_midi_raw(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;

    MidiEvent event;
    for (int i = 0; i < 3; ++i){
        event.data[i] = atom_getfloatarg(i, argc, argv);
    }
    event.delta = x->get_sample_offset();
    x->x_plugin->sendMidiEvent(event);
}

// helper function
static void vstplugin_midi_mess(t_vstplugin *x, int onset, int channel, int d1, int d2 = 0, float detune = 0){
    if (!x->check_plugin()) return;

    channel = std::max(1, std::min(16, (int)channel)) - 1;
    d1 = std::max(0, std::min(127, d1));
    d2 = std::max(0, std::min(127, d2));
    x->x_plugin->sendMidiEvent(MidiEvent(onset + channel, d1, d2, x->get_sample_offset(), detune));
}

// send MIDI messages (convenience methods)
static void vstplugin_midi_noteoff(t_vstplugin *x, t_floatarg channel, t_floatarg pitch, t_floatarg velocity){
    float detune = (pitch - static_cast<int>(pitch)) * 100.f;
    vstplugin_midi_mess(x, 128, channel, pitch, velocity, detune);
}

static void vstplugin_midi_note(t_vstplugin *x, t_floatarg channel, t_floatarg pitch, t_floatarg velocity){
    float detune = (pitch - static_cast<int>(pitch)) * 100.f;
    vstplugin_midi_mess(x, 144, channel, pitch, velocity, detune);
}

static void vstplugin_midi_polytouch(t_vstplugin *x, t_floatarg channel, t_floatarg pitch, t_floatarg pressure){
    vstplugin_midi_mess(x, 160, channel, pitch, pressure);
}

static void vstplugin_midi_cc(t_vstplugin *x, t_floatarg channel, t_floatarg ctl, t_floatarg value){
    vstplugin_midi_mess(x, 176, channel, ctl, value);
}

static void vstplugin_midi_program(t_vstplugin *x, t_floatarg channel, t_floatarg program){
   vstplugin_midi_mess(x, 192, channel, program);
}

static void vstplugin_midi_touch(t_vstplugin *x, t_floatarg channel, t_floatarg pressure){
    vstplugin_midi_mess(x, 208, channel, pressure);
}

static void vstplugin_midi_bend(t_vstplugin *x, t_floatarg channel, t_floatarg bend){
    // map from [-1.f, 1.f] to [0, 16383] (14 bit)
    int val = (bend + 1.f) * 8192.f; // 8192 is the center position
    val = std::max(0, std::min(16383, val));
    vstplugin_midi_mess(x, 224, channel, val & 127, (val >> 7) & 127);
}

// send MIDI SysEx message
static void vstplugin_midi_sysex(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;

    std::string data;
    data.reserve(argc);
    for (int i = 0; i < argc; ++i){
        data.push_back((unsigned char)atom_getfloat(argv+i));
    }

    x->x_plugin->sendSysexEvent(SysexEvent(data.data(), data.size()));
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~ programs ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

// set the current program by index
static void vstplugin_program_set(t_vstplugin *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->info().numPrograms()){
        x->x_plugin->setProgram(index);
        x->x_editor->update(true);
    } else {
        pd_error(x, "%s: program number %d out of range!", classname(x), index);
    }
}

// get the current program index
static void vstplugin_program_get(t_vstplugin *x){
    if (!x->check_plugin()) return;
    t_atom msg;
    SETFLOAT(&msg, x->x_plugin->getProgram());
    outlet_anything(x->x_messout, gensym("program"), 1, &msg);
}

// set the name of the current program
static void vstplugin_program_name_set(t_vstplugin *x, t_symbol* name){
    if (!x->check_plugin()) return;
    x->x_plugin->setProgramName(name->s_name);
}

// get the program name by index. no argument: get the name of the current program.
static void vstplugin_program_name_get(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    t_atom msg[2];
    if (argc){
        int index = atom_getfloat(argv);
        SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getProgramNameIndexed(index).c_str()));
    } else {
        SETFLOAT(&msg[0], x->x_plugin->getProgram());
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getProgramName().c_str()));
    }
    outlet_anything(x->x_messout, gensym("program_name"), 2, msg);
}

// get number of programs
static void vstplugin_program_count(t_vstplugin *x){
    if (!x->check_plugin()) return;
    t_atom msg;
    SETFLOAT(&msg, x->x_plugin->info().numPrograms());
    outlet_anything(x->x_messout, gensym("program_count"), 1, &msg);
}

// list all programs (index + name)
static void vstplugin_program_list(t_vstplugin *x,  t_symbol *s){
    const PluginDesc *info = nullptr;
    bool local = false;
    if (*s->s_name){
        auto abspath = x->resolve_plugin_path(s->s_name);
        if (abspath.empty() || !(info = queryPlugin<false>(abspath))){
            pd_error(x, "%s: couldn't open '%s' - no such file or plugin!",
                     classname(x), s->s_name);
            return;
        }
    } else {
        if (!x->check_plugin()) return;
        info = &x->x_plugin->info();
        local = true;
    }
    int n = info->numPrograms();
    t_atom msg[2];
    for (int i = 0; i < n; ++i){
        t_symbol *name = gensym(local ? x->x_plugin->getProgramNameIndexed(i).c_str()
                                            : info->programs[i].c_str());
        SETFLOAT(&msg[0], i);
        SETSYMBOL(&msg[1], name);
        outlet_anything(x->x_messout, gensym("program_name"), 2, msg);
    }
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~ presets ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

enum t_preset {
    PROGRAM,
    BANK,
    PRESET
};

static const char * presetName(t_preset type){
    static const char* names[] = { "program", "bank", "preset" };
    return names[type];
}

struct t_preset_data : t_command_data<t_preset_data> {
    std::string path;
    std::string errmsg;
    bool success;
};

/*----------------------- "preset_data_set" -------------------------*/

// set program/bank data (list of bytes)
// TODO: make this asynchronous!
template<t_preset type>
static void vstplugin_preset_data_set(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    std::string buffer;
    buffer.resize(argc);
    for (int i = 0; i < argc; ++i){
        // first clamp to 0-255, then assign to char (not 100% portable...)
        buffer[i] = (unsigned char)atom_getfloat(argv + i);
    }
    try {
        x->x_editor->defer_safe<false>([&](){
            t_scoped_nrt_lock lock(x->x_mutex); // avoid concurrent reads/writes
            if (type == BANK)
                x->x_plugin->readBankData(buffer);
            else
                x->x_plugin->readProgramData(buffer);
        }, x->x_uithread);
        x->x_editor->update(false);
    } catch (const Error& e) {
        pd_error(x, "%s: couldn't set %s data: %s",
                 classname(x), presetName(type), e.what());
    }
}

/*----------------------- "preset_data_get" ----------------------------*/

// get program/bank data
// TODO: make this asynchronous!
template<t_preset type>
static void vstplugin_preset_data_get(t_vstplugin *x){
    if (!x->check_plugin()) return;
    std::string buffer;
    try {
       x->x_editor->defer_safe<false>([&](){
            t_scoped_nrt_lock lock(x->x_mutex); // avoid concurrent reads/writes
            if (type == BANK)
                x->x_plugin->writeBankData(buffer);
            else
                x->x_plugin->writeProgramData(buffer);
        }, x->x_uithread);
    } catch (const Error& e){
        pd_error(x, "%s: couldn't get %s data: %s",
                 classname(x), presetName(type), e.what());
        return;
    }
    const int n = buffer.size();
    std::vector<t_atom> atoms;
    atoms.resize(n);
    for (int i = 0; i < n; ++i){
            // first convert to range 0-255, then assign to t_float (not 100% portable...)
        SETFLOAT(&atoms[i], (unsigned char)buffer[i]);
    }
    outlet_anything(x->x_messout, gensym(type == BANK ? "bank_data" : "program_data"),
                    n, atoms.data());
}

/*-------------------------- "program_read" ----------------------------*/

// read program/bank file (.FXP/.FXB)
template<t_preset type, bool async>
static void vstplugin_preset_read_do(t_preset_data *data){
    try {
        auto x = data->owner;
        // NOTE: avoid readProgramFile() to minimize the critical section
        vst::File file(data->path);
        if (!file.is_open()){
            throw Error("couldn't open file");
        }
        auto buffer = file.readAll();
        if (file){
            LOG_DEBUG("successfully read " << buffer.size() << " bytes");
        } else {
            throw Error("couldn't read file");
        }

        x->x_editor->defer_safe<async>([&](){
            // protect against vstplugin_dsp() and vstplugin_save()
            t_scoped_nrt_lock lock(x->x_mutex);
            if (type == BANK)
                x->x_plugin->readBankData(buffer);
            else
                x->x_plugin->readProgramData(buffer);
        }, x->x_uithread);

        data->success = true;
    } catch (const Error& e) {
        data->errmsg = e.what();
        data->success = false;
    }
}

template<t_preset type>
static void vstplugin_preset_read_notify(t_vstplugin *x, bool success) {
    t_atom a;
    SETFLOAT(&a, success);
    char sel[64];
    snprintf(sel, sizeof(sel), "%s_read", presetName(type));
    outlet_anything(x->x_messout, gensym(sel), 1, &a);
}

template<t_preset type>
static void vstplugin_preset_read_done(t_preset_data *data){
    auto x = data->owner;
    if (!data->success) {
        pd_error(x, "%s: could not read %s '%s':\n%s",
                 classname(x), presetName(type),
                 data->path.c_str(), data->errmsg.c_str());
    }
    // command finished
    x->x_suspended = false;
    // *now* update
    x->x_editor->update(false);

    vstplugin_preset_read_notify<type>(x, data->success);
}

template<t_preset type>
static void vstplugin_preset_read(t_vstplugin *x, t_symbol *s, t_float f){
    if (!x->check_plugin()) return;

    // resolve path
    char path[MAXPDSTRING];
    if (sys_isabsolutepath(s->s_name)){
        snprintf(path, MAXPDSTRING, "%s", s->s_name);
    } else {
        char dir[MAXPDSTRING], *name;
        int fd = canvas_open(x->x_canvas, s->s_name, "", dir, &name, MAXPDSTRING, 1);
        if (fd >= 0) {
            snprintf(path, MAXPDSTRING, "%s/%s", dir, name);
            sys_close(fd);
        } else {
            pd_error(x, "%s: could not find %s '%s'",
                     classname(x), presetName(type), s->s_name);
            vstplugin_preset_read_notify<type>(x, false);
            return;
        }
    }

    bool async = (f != 0);
    if (async){
        auto data = new t_preset_data();
        data->owner = x;
        data->path = path;
        t_workqueue::get()->push(x, data, vstplugin_preset_read_do<type, true>,
                                 vstplugin_preset_read_done<type>);
        x->x_suspended = true;
    } else {
        t_preset_data data;
        data.owner = x;
        data.path = path;
        vstplugin_preset_read_do<type, false>(&data);
        vstplugin_preset_read_done<type>(&data);
    }
}

/*-------------------------- "program_write" ----------------------------*/

struct t_save_data : t_preset_data {
    std::string name;
    PresetType type;
    bool add;
};

// write program/bank file (.FXP/.FXB)
template<t_preset type, bool async>
static void vstplugin_preset_write_do(t_preset_data *data){
    try {
        auto x = data->owner;
        // NOTE: we avoid writeProgram() to minimize the critical section
        std::string buffer;
        if (async){
            // try to move memory allocation *before* the lock,
            // so we keep the critical section as short as possible.
            buffer.reserve(1024);
        }
        // protect against vstplugin_dsp() and vstplugin_save()
        x->x_editor->defer_safe<async>([&](){
            t_scoped_nrt_lock lock(x->x_mutex);
            if (type == BANK)
                x->x_plugin->writeBankData(buffer);
            else
                x->x_plugin->writeProgramData(buffer);
        }, x->x_uithread);
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
        data->success = true;
    } catch (const Error& e){
        data->errmsg = e.what();
        data->success = false;
    }
}

static void vstplugin_preset_change_notify(t_vstplugin *x);

template<t_preset type>
static void vstplugin_preset_write_done(t_preset_data *data){
    auto x = data->owner;
    if (!data->success) {
        pd_error(x, "%s: couldn't write %s file '%s':\n%s",
                 classname(x), presetName(type),
                 data->path.c_str(), data->errmsg.c_str());
    }
    // command finished
    x->x_suspended = false;
    if (type == PRESET && data->success){
        auto y = (t_save_data *)data;
        // set current preset
        x->x_preset = gensym(y->name.c_str());
        // add preset and notify for change
        if (y->add){
            auto& info = x->x_plugin->info();
            Preset preset;
            preset.name = y->name;
            preset.path = y->path;
            preset.type = y->type;
            {
            #ifdef PDINSTANCE
                std::lock_guard lock(gPresetMutex);
            #endif
                const_cast<PluginDesc&>(info).addPreset(std::move(preset));
            }
            vstplugin_preset_change_notify(x);
        }
    }
    // notify
    t_atom a;
    SETFLOAT(&a, data->success);
    char sel[64];
    snprintf(sel, sizeof(sel), "%s_write", presetName(type));
    outlet_anything(x->x_messout, gensym(sel), 1, &a);
}

template<t_preset type>
static void vstplugin_preset_write(t_vstplugin *x, t_symbol *s, t_floatarg f){
    if (!x->check_plugin()) return;
    // get the full path here because it's relatively cheap,
    // otherwise we would have to lock Pd in the NRT thread
    // (like we do in vstplugin_preset_read)
    char path[MAXPDSTRING];
    canvas_makefilename(x->x_canvas, s->s_name, path, MAXPDSTRING);

    bool async = (f != 0);
    if (async){
        auto data = new t_preset_data();
        data->owner = x;
        data->path = path;
        t_workqueue::get()->push(x, data, vstplugin_preset_write_do<type, true>,
                                 vstplugin_preset_write_done<type>);
        x->x_suspended = true;
    } else {
        t_preset_data data;
        data.owner = x;
        data.path = path;
        vstplugin_preset_write_do<type, false>(&data);
        vstplugin_preset_write_done<type>(&data);
    }
}

/*-------------------------- "preset_count" --------------------------*/

static void vstplugin_preset_count(t_vstplugin *x){
    if (!x->check_plugin()) return;
    auto& info = x->x_plugin->info();
    t_atom msg;
    {
    #ifdef PDINSTANCE
        std::shared_lock lock(gPresetMutex);
    #endif
        SETFLOAT(&msg, info.numPresets());
    }
    outlet_anything(x->x_messout, gensym("preset_count"), 1, &msg);
}

/*-------------------------- "preset_info" ---------------------------*/

static void vstplugin_preset_doinfo(t_vstplugin *x, const PluginDesc& info, int index){
#ifdef PDINSTANCE
    // Note that another Pd instance might modify the preset list while we're iterating and
    // outputting the presets. Since we have to unlock before sending to the outlet to avoid
    // deadlocks, I don't see what we can do... maybe add a recursion check?
    // At least we always do a range check.
    std::shared_lock lock(gPresetMutex);
#endif
    if (index >= 0 && index < info.numPresets()){
        auto& preset = info.presets[index];
        int type = 0;
        switch (preset.type){
        case PresetType::User:
            type = 0;
            break;
        case PresetType::UserFactory:
            type = 1;
            break;
        case PresetType::SharedFactory:
            type = 2;
            break;
        case PresetType::Global:
            type = 3;
            break;
        default:
            bug("vstplugin_preset_info");
            break;
        }
        t_atom msg[4];
        SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(preset.name.c_str()));
        SETSYMBOL(&msg[2], gensym(preset.path.c_str()));
        SETFLOAT(&msg[3], type);
    #ifdef PDINSTANCE
        lock.unlock(); // !
    #endif
        outlet_anything(x->x_messout, gensym("preset_info"), 4, msg);
    } else {
        pd_error(x, "%s: preset index %d out of range!", classname(x), index);
    }
}

static void vstplugin_preset_info(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    vstplugin_preset_doinfo(x, x->x_plugin->info(), (int)f);
}

/*-------------------------- "preset_list" ----------------------------*/

static void vstplugin_preset_list(t_vstplugin *x, t_symbol *s){
    const PluginDesc *info = nullptr;
    if (*s->s_name){
        auto abspath = x->resolve_plugin_path(s->s_name);
        if (abspath.empty() || !(info = queryPlugin<false>(abspath))){
            pd_error(x, "%s: couldn't open '%s' - no such file or plugin!",
                     classname(x), s->s_name);
            return;
        }
    } else {
        if (!x->check_plugin()) return;
        info = &x->x_plugin->info();
    }
    int numpresets;
    {
    #ifdef PDINSTANCE
        // see comment in vstplugin_preset_doinfo
        std::shared_lock lock(gPresetMutex);
    #endif
        numpresets = info->numPresets();
    }
    for (int i = 0; i < numpresets; ++i){
        vstplugin_preset_doinfo(x, *info, i);
    }
}

static int vstplugin_preset_index(t_vstplugin *x, int argc, t_atom *argv, bool loud = true){
    if (argc){
        switch (argv->a_type){
        case A_FLOAT:
            {
                int index = argv->a_w.w_float;
                if (index >= 0 && index < x->x_plugin->info().numPresets()){
                    return index;
                } else if (index == -1){
                    // current preset
                    if (x->x_preset){
                        index = x->x_plugin->info().findPreset(x->x_preset->s_name);
                        if (index >= 0){
                            return index;
                        } else {
                            pd_error(x, "%s: couldn't find (current) preset '%s'!",
                                     classname(x), x->x_preset->s_name);
                        }
                    } else {
                        pd_error(x, "%s: no current preset!", classname(x));
                    }
                } else {
                    pd_error(x, "%s: preset index %d out of range!", classname(x), index);
                }
            }
            break;
        case A_SYMBOL:
            {
                t_symbol *s = argv->a_w.w_symbol;
                if (*s->s_name){
                    int index = x->x_plugin->info().findPreset(s->s_name);
                    if (index >= 0 || !loud){
                        return index;
                    } else {
                        pd_error(x, "%s: couldn't find preset '%s'!", classname(x), s->s_name);
                    }
                } else {
                    pd_error(x, "%s: bad preset name!", classname(x));
                }
                break;
            }
        default:
            pd_error(x, "%s: bad atom type for preset!", classname(x));
            break;
        }
    } else {
        pd_error(x, "%s: missing preset argument!", classname(x));
    }
    return -1;
}

static bool vstplugin_preset_writeable(t_vstplugin *x, const PluginDesc& info, int index){
    bool writeable = info.presets[index].type == PresetType::User;
    if (!writeable){
        pd_error(x, "%s: preset is not writeable!", classname(x));
    }
    return writeable;
}


static void vstplugin_preset_change_notify(t_vstplugin *x){
    auto thing = gensym(t_vstplugin::glob_recv_name)->s_thing;
    if (thing){
        // notify all vstplugin~ instances for preset changes
        pd_vmess(thing, gensym("preset_change"), (char *)"s", x->x_key);
    }
}

static void vstplugin_preset_change(t_vstplugin *x, t_symbol *s){
    // only forward message to matching instances
    if (s == x->x_key){
        t_atom a;
        SETSYMBOL(&a, s);
        outlet_anything(x->x_messout, gensym("preset_change"), 1, &a);
    }
}

/*-------------------------- "preset_load" ----------------------------*/

static void vstplugin_preset_load(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;

    auto& info = x->x_plugin->info();
    t_symbol *path;
    {
    #ifdef PDINSTANCE
        std::shared_lock lock(gPresetMutex);
    #endif
        int index = vstplugin_preset_index(x, argc, argv);
        if (index < 0){
            t_atom a;
            SETFLOAT(&a, 0);
        #ifdef PDINSTANCE
            lock.unlock(); // !
        #endif
            outlet_anything(x->x_messout, gensym("preset_load"), 1, &a);
            return;
        }

        auto& preset = info.presets[index];
        x->x_preset = gensym(preset.name.c_str());
        path = gensym(preset.path.c_str());
    }

    bool async = atom_getfloatarg(1, argc, argv); // optional 2nd argument
    vstplugin_preset_read<PRESET>(x, path, async);
}

/*-------------------------- "preset_save" ----------------------------*/

static void vstplugin_preset_save(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;

    auto& info = x->x_plugin->info();
#ifdef PDINSTANCE
    std::unique_lock lock(gPresetMutex);
#endif
    Preset preset;
    bool add = false;
    int index = vstplugin_preset_index(x, argc, argv, false);
    // existing preset must be writeable!
    if (index >= 0 && argv->a_type == A_FLOAT && !vstplugin_preset_writeable(x, info, index)){
        t_atom a;
        SETFLOAT(&a, 0);
    #ifdef PDINSTANCE
        lock.unlock(); // !
    #endif
        outlet_anything(x->x_messout, gensym("preset_save"), 1, &a);
        return;
    }
    // if the preset *name* couldn't be found, make a new preset
    if (index < 0){
        t_symbol *name = atom_getsymbolarg(0, argc, argv);
        if (*name->s_name){
            preset = info.makePreset(name->s_name);
            add = true;
        } else {
            t_atom a;
            SETFLOAT(&a, 0);
        #ifdef PDINSTANCE
            lock.unlock(); // !
        #endif
            outlet_anything(x->x_messout, gensym("preset_save"), 1, &a);
            return;
        }
    } else {
        preset = info.presets[index];
    }

    bool async = atom_getfloatarg(1, argc, argv); // optional 2nd argument
    if (async){
        auto data = new t_save_data();
        data->owner = x;
        data->name = std::move(preset.name);
        data->path = std::move(preset.path);
        data->type = preset.type;
        data->add = add;
        t_workqueue::get()->push(x, data,
                                 vstplugin_preset_write_do<PRESET, true>,
                                 vstplugin_preset_write_done<PRESET>);
        x->x_suspended = true;
    } else {
        t_save_data data;
        data.owner = x;
        data.name = std::move(preset.name);
        data.path = std::move(preset.path);
        data.type = preset.type;
        data.add = add;
    #ifdef PDINSTANCE
        lock.unlock(); // to avoid deadlock in vstplugin_preset_write_done
    #endif
        vstplugin_preset_write_do<PRESET, false>(&data);
        vstplugin_preset_write_done<PRESET>(&data);
    }
}

/*-------------------------- "preset_rename" --------------------------*/

struct t_rename_data : t_command_data<t_rename_data> {
    int index;
    t_symbol *newname;
    bool update;
    bool success;
};

// LATER think about a proper async version without causing too much trouble
static void vstplugin_preset_rename(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    auto& info = x->x_plugin->info();
#ifdef PDINSTANCE
    std::unique_lock lock(gPresetMutex);
#endif

    auto notify = [](t_vstplugin *x, bool result){
        t_atom a;
        SETFLOAT(&a, result);
        outlet_anything(x->x_messout, gensym("preset_rename"), 1, &a);
    };

    // 1) preset
    int index = vstplugin_preset_index(x, (argc > 1), argv);
    if (index < 0){
    #ifdef PDINSTANCE
        lock.unlock(); // !
    #endif
        notify(x, false);
        return;
    }
    // 2) new name
    t_symbol *newname = atom_getsymbolarg(1, argc, argv);
    if (!(*newname->s_name)){
        pd_error(x, "%s: bad preset name %s!", classname(x), newname->s_name);
    #ifdef PDINSTANCE
        lock.unlock(); // !
    #endif
        notify(x, false);
        return;
    }
    // check if we rename the current preset
    bool update = x->x_preset && (x->x_preset->s_name == info.presets[index].name);

    if (vstplugin_preset_writeable(x, info, index)){
        if (const_cast<PluginDesc&>(info).renamePreset(index, newname->s_name)){
            if (update){
                x->x_preset = newname;
            }
        #ifdef PDINSTANCE
            lock.unlock(); // !
        #endif
            vstplugin_preset_change_notify(x);
            notify(x, true);
            return; // success
        } else {
            pd_error(x, "%s: couldn't rename preset!", classname(x));
        }
    }
    notify(x, false);
}

/*-------------------------- "preset_delete" --------------------------*/

struct t_delete_data : t_command_data<t_delete_data> {
    int index;
    bool current;
    bool success;
};

// LATER think about a proper async version without causing too much trouble
static void vstplugin_preset_delete(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    auto& info = x->x_plugin->info();
#ifdef PDINSTANCE
    std::unique_lock lock(gPresetMutex);
#endif

    auto notify = [](t_vstplugin *x, bool result){
        t_atom a;
        SETFLOAT(&a, result);
        outlet_anything(x->x_messout, gensym("preset_delete"), 1, &a);
    };

    int index = vstplugin_preset_index(x, argc, argv);
    if (index < 0){
    #ifdef PDINSTANCE
        lock.unlock(); // !
    #endif
        notify(x, false);
        return;
    }

    // check if we delete the current preset
    bool current = x->x_preset && (x->x_preset->s_name == info.presets[index].name);

    if (vstplugin_preset_writeable(x, info, index)){
        if (const_cast<PluginDesc&>(info).removePreset(index)){
            if (current){
                x->x_preset = nullptr;
            }
        #ifdef PDINSTANCE
            lock.unlock(); // !
        #endif
            vstplugin_preset_change_notify(x);
            notify(x, true);
            return; // success
        } else {
            pd_error(x, "%s: couldn't delete preset!", classname(x));
        }
    }
    notify(x, false);
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~ helper methods ~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

static t_class *vstplugin_class;

// automated is true if parameter was set from the (generic) GUI, false if set by message ("param_set")
void t_vstplugin::set_param(int index, float value, bool automated){
    if (index >= 0 && index < x_plugin->info().numParameters()){
        value = std::max(0.f, std::min(1.f, value));
        int offset = x_plugin->info().type() == PluginType::VST3 ? get_sample_offset() : 0;
        x_plugin->setParameter(index, value, offset);
        if (deferred()) {
            x_editor->param_changed_deferred(index, automated);
        } else {
            x_editor->param_changed(index, value, automated);
        }
    } else {
        pd_error(this, "%s: parameter index %d out of range!", classname(this), index);
    }
}

void t_vstplugin::set_param(int index, const char *s, bool automated){
    if (index >= 0 && index < x_plugin->info().numParameters()){
        int offset = x_plugin->info().type() == PluginType::VST3 ? get_sample_offset() : 0;
        if (!x_plugin->setParameter(index, s, offset)){
            pd_error(this, "%s: bad string value for parameter %d!", classname(this), index);
            // NB: some plugins don't just ignore bad string input, but reset the parameter to some value...
        }
        if (deferred()) {
            x_editor->param_changed_deferred(index, automated);
        } else {
            x_editor->param_changed(index, x_plugin->getParameter(index), automated);
        }
    } else {
        pd_error(this, "%s: parameter index %d out of range!", classname(this), index);
    }
}

bool t_vstplugin::check_plugin(){
    if (x_plugin){
        if (!x_suspended){
            return true;
        } else {
            pd_error(this, "%s: temporarily suspended!", classname(this));
        }
    } else {
        pd_error(this, "%s: no plugin loaded!", classname(this));
    }
    return false;
}

void t_vstplugin::setup_plugin(IPlugin& plugin){
    // check processing precision before calling setupProcessing()!
    // (only post warnings in vstplugin_open_done())
    if (plugin.info().hasPrecision(x_wantprecision)){
        x_realprecision = x_wantprecision;
        x_process = true;
    } else {
        if (plugin.info().hasPrecision(ProcessPrecision::Single)){
            x_realprecision = ProcessPrecision::Single;
            x_process = true;
        } else if (plugin.info().hasPrecision(ProcessPrecision::Double)){
            x_realprecision = ProcessPrecision::Double;
            x_process = true;
        } else {
            x_process = false; // bypass
        }
    }

    plugin.suspend();
    plugin.setupProcessing(x_sr, x_blocksize, x_realprecision, x_mode);

    auto setupSpeakers = [](const auto& pluginBusses,
            const auto& ugenBusses, auto& result, const char *what) {
        assert(ugenBusses.size() >= 1);
        result.resize(pluginBusses.size());

        if (ugenBusses.size() == 1 && pluginBusses.size() > 1){
            LOG_DEBUG("distribute " << what);
            // distribute inlets/outlets over plugin busses
            //
            // NOTE: only do this if the plugin has more than one bus,
            // as a workaround for buggy VST3 plugins which would report
            // a wrong default channel count, like Helm.vst3 or RoughRider2.vst3
            auto remaining = ugenBusses[0].b_n;
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
            LOG_DEBUG("associate " << what);
            // associate inlet/outlet busses with plugin input/output busses.
            for (int i = 0; i < (int)pluginBusses.size(); ++i){
                if (i < (int)ugenBusses.size()){
                    result[i] = ugenBusses[i].b_n;
                } else {
                    result[i] = 0;
                }
            }
        }
    };

    // prepare input busses
    std::vector<int> inputs;
    setupSpeakers(plugin.info().inputs, x_inlets, inputs, "inputs");

    // prepare output busses
    std::vector<int> outputs;
    setupSpeakers(plugin.info().outputs, x_outlets, outputs, "outputs");

    plugin.setNumSpeakers(inputs.data(), inputs.size(),
                          outputs.data(), outputs.size());

    x_inputs.resize(inputs.size());
    x_input_channels = 0;
    for (int i = 0; i < (int)inputs.size(); ++i){
        x_input_channels += inputs[i];
        x_inputs[i] = Bus(inputs[i]);
    }

    x_outputs.resize(outputs.size());
    x_output_channels = 0;
    for (int i = 0; i < (int)outputs.size(); ++i){
        x_output_channels += outputs[i];
        x_outputs[i] = Bus(outputs[i]);
    }

    plugin.resume();
}

void t_vstplugin::update_buffers(){
    int samplesize;
    if (x_plugin) {
        samplesize = (x_realprecision == ProcessPrecision::Double) ?
                    sizeof(double) : sizeof(float);
    } else {
        samplesize = sizeof(t_sample);
    }
    int channelsize = samplesize * x_blocksize;

    // prepare inlets
    // NOTE: we always have to buffer the inlets!
    int ninchannels = 0;
    for (int i = 0; i < (int)x_inlets.size(); ++i){
        ninchannels += x_inlets[i].b_n;
    }
    ninchannels++; // extra dummy buffer
    x_inbuffer.resize(ninchannels * channelsize);
    auto inbuf = x_inbuffer.data();
    auto indummy = inbuf;
    memset(indummy, 0, channelsize); // zero!
    inbuf += channelsize;
    // set inlet buffer pointers
    for (auto& inlets : x_inlets){
        for (int i = 0; i < inlets.b_n; ++i, inbuf += channelsize){
            inlets.b_buffers[i] = inbuf;
        }
    }

    // prepare outlets
    // NOTE: only buffer the outlets if Pd and VST plugin
    // use a different float size!
    bool needbuffer = (samplesize != sizeof(t_sample));
    int noutchannels = 0;
    if (needbuffer){
        for (int i = 0; i < (int)x_outlets.size(); ++i){
            noutchannels += x_outlets[i].b_n;
        }
    }
    noutchannels++; // extra dummy buffer
    x_outbuffer.resize(noutchannels * channelsize);
    auto outbuf = x_outbuffer.data();
    auto outdummy = outbuf;
    outbuf += channelsize;
    if (needbuffer){
        // set outlet buffer pointers
        for (auto& outlets : x_outlets){
            for (int i = 0; i < outlets.b_n; ++i, outbuf += channelsize){
                outlets.b_buffers[i] = outbuf;
            }
        }
    }

    auto setupBusses = [](auto& ugenBusses, auto& pluginBusses,
            auto* dummy, bool needBuffer, const char *what) {
        assert(ugenBusses.size() >= 1);
        if (ugenBusses.size() == 1 && pluginBusses.size() > 1){
            LOG_DEBUG("distribute " << what);
            // distribute inlets/outlets over plugin busses
            //
            // NOTE: only do this if the plugin has more than one bus,
            // as a workaround for buggy VST3 plugins which would report
            // a wrong default channel count, like Helm.vst3 or RoughRider2.vst3
            auto& buffers = ugenBusses[0].b_buffers;
            auto& signals = ugenBusses[0].b_signals;
            auto nchannels = ugenBusses[0].b_n;
            int index = 0;
            for (int i = 0; i < (int)pluginBusses.size(); ++i){
                auto& bus = pluginBusses[i];
                // LOG_DEBUG("set bus " << i);
                for (int j = 0; j < bus.numChannels; ++j, ++index){
                    if (index < nchannels){
                        if (needBuffer){
                            // point to inlet/outlet buffer
                            // LOG_DEBUG("channel " << j << ": point to buffer");
                            bus.channelData32[j] = (float *)buffers[index];
                        } else {
                            // directly point to inlet/outlet
                            // LOG_DEBUG("channel " << j << ": point to signal");
                            bus.channelData32[j] = (float *)signals[index];
                        }
                    } else {
                        // point to dummy
                        // LOG_DEBUG("channel " << j << ": point to dummy");
                        bus.channelData32[j] = (float *)dummy;
                    }
                }
            }
        } else {
            LOG_DEBUG("associate " << what);
            // associate inlet/outlet busses with plugin input/output busses.
            for (int i = 0; i < (int)pluginBusses.size(); ++i){
                auto& bus = pluginBusses[i];
                // LOG_DEBUG("set bus " << i);
                if (i < (int)ugenBusses.size()){
                    for (int j = 0; j < bus.numChannels; ++j){
                        if (j < ugenBusses[i].b_n){
                            if (needBuffer){
                                // point to inlet/outlet buffer
                                // LOG_DEBUG("channel " << j << ": point to buffer");
                                bus.channelData32[j] = (float *)ugenBusses[i].b_buffers[j];
                            } else {
                                // directly point to inlet/outlet
                                // LOG_DEBUG("channel " << j << ": point to signal");
                                bus.channelData32[j] = (float *)ugenBusses[i].b_signals[j];
                            }
                        } else {
                            // point to dummy
                            // LOG_DEBUG("channel " << j << ": point to dummy");
                            bus.channelData32[j] = (float *)dummy;
                        }
                    }
                } else {
                    // point all channels to dummy
                    for (int j = 0; j < bus.numChannels; ++j){
                        // LOG_DEBUG("channel " << j << ": point to dummy");
                        bus.channelData32[j] = (float *)dummy;
                    }
                }
            }
        }
    };

    // setup plugin inputs
    setupBusses(x_inlets, x_inputs, indummy, true, "inlets"); // always buffer!
    // setup plugin outputs
    setupBusses(x_outlets, x_outputs, outdummy, needbuffer, "outlets");
}

int t_vstplugin::get_sample_offset(){
    int offset = clock_gettimesincewithunits(x_lastdsptime, 1, true);
    // LOG_DEBUG("sample offset: " << offset);
    return offset % x_blocksize;
}

std::string t_vstplugin::resolve_plugin_path(const char *s) {
    // first try plugin dictionary
    auto desc = gPluginDict.findPlugin(s);
    if (desc) {
        return s; // return key/path
    }
    // try as file path
    if (sys_isabsolutepath(s)){
        return normalizePath(s); // success!
    } else {
        // resolve relative path to canvas search paths or VST search paths
        bool vst3 = false;
        std::string path = s;
        auto ext = fileExtension(path);
        if (ext == ".vst3"){
            vst3 = true;
        } else if (ext.empty()){
            // no extension: assume VST2 plugin
        #ifdef _WIN32
            path += ".dll";
        #elif defined(__APPLE__)
            path += ".vst";
        #else // Linux/BSD/etc.
            path += ".so";
        #endif
        }
            // first try canvas search paths
        char fullPath[MAXPDSTRING];
        char dirresult[MAXPDSTRING];
        char *name = nullptr;
        int fd;
    #ifdef __APPLE__
        const char *bundlePath = "Contents/Info.plist";
        // on MacOS VST plugins are always bundles (directories) but canvas_open needs a real file
        snprintf(fullPath, MAXPDSTRING, "%s/%s", path.c_str(), bundlePath);
        fd = canvas_open(x_canvas, fullPath, "", dirresult, &name, MAXPDSTRING, 1);
    #else
        const char *bundlePath = nullptr;
        fd = canvas_open(x_canvas, path.c_str(), "", dirresult, &name, MAXPDSTRING, 1);
        if (fd < 0 && vst3){
            // VST3 plugins might be bundles
            // NOTE: this doesn't work for bridged plugins (yet)!
            bundlePath = getBundleBinaryPath();
        #ifdef _WIN32
            snprintf(fullPath, MAXPDSTRING, "%s/%s/%s",
                     path.c_str(), bundlePath, fileName(path).c_str());
        #else
            snprintf(fullPath, MAXPDSTRING, "%s/%s/%s.so",
                     path.c_str(), bundlePath, fileBaseName(path).c_str());
         #endif
            fd = canvas_open(x_canvas, fullPath, "", dirresult, &name, MAXPDSTRING, 1);
        }
    #endif
        if (fd >= 0){
            sys_close(fd);
            char buf[MAXPDSTRING+1];
            snprintf(buf, MAXPDSTRING, "%s/%s", dirresult, name);
            if (bundlePath){
                // restore original path
                char *pos = strstr(buf, bundlePath);
                if (pos){
                    pos[-1] = 0;
                }
            }
            return normalizePath(buf); // success!
        } else {
            // otherwise try default VST paths
            for (auto& dir : getDefaultSearchPaths()){
                auto result = vst::find(dir, path);
                if (!result.empty()) {
                    return normalizePath(result); // success
                }
            }
        }
    }
    pd_error(this, "'%s' is neither an existing plugin name nor a valid file path", s);
    return "";
}

/*---------------------------- constructor -----------------------------*/

// usage: vstplugin~ [flags...] [file] inlets (default=2) outlets (default=2)
t_vstplugin::t_vstplugin(int argc, t_atom *argv){
    t_workqueue::init();

    bool search = false; // search for plugins in the standard VST directories
    bool gui = true; // use GUI?
    bool threaded = false;
    auto mode = RunMode::Auto;
    // precision (defaults to Pd's precision)
    ProcessPrecision precision = (PD_FLOATSIZE == 64) ?
                ProcessPrecision::Double : ProcessPrecision::Single;
    t_symbol *file = nullptr; // plugin to open (optional)
    bool editor = false; // open plugin with VST editor?
    std::vector<int> inputs;
    std::vector<int> outputs;

    auto parseBusses = [this](t_atom *& argv, int& argc, const char *flag){
        std::vector<int> result;
        argv++; argc--;

        if (argc > 0 && argv->a_type == A_FLOAT){
            int n = argv->a_w.w_float;
            argv++; argc--;
            for (int i = 0; i < n; ++i){
                if (argc > 0){
                    if (argv->a_type == A_FLOAT){
                        int chn = argv->a_w.w_float;
                        if (chn < 0){
                            pd_error(this, "%s: negative channel number for bus %d",
                                     classname(this), i);
                            chn = 0;
                        }
                        result.push_back(argv->a_w.w_float);
                    } else {
                        pd_error(this, "%s: bad channel argument %s for bus %d",
                                 classname(this), atom_getsymbol(argv)->s_name, i);
                    }
                    argv++; argc--;
                } else {
                    pd_error(this, "%s: missing channel argument for bus %d",
                             classname(this), i);
                }
            }
        } else {
            pd_error(this, "%s: too few arguments for %s flag",
                     classname(this), flag);
        }
        // we need at least a single bus!
        if (result.empty()){
            result.push_back(0);
        }
        return result;
    };

    while (argc && argv->a_type == A_SYMBOL){
        const char *flag = argv->a_w.w_symbol->s_name;
        if (*flag == '-'){
            if (!strcmp(flag, "-n")){
                gui = false;
            } else if (!strcmp(flag, "-m")){
                if (g_signal_setmultiout) {
                    x_multi = true;
                } else {
                    pd_error(this, "%s: no multi-channel support, ignoring '-m' flag", classname(this));
                }
            } else if (!strcmp(flag, "-i")){
                inputs = parseBusses(argv, argc, "-i");
                // we always have at least 1 inlet because of CLASS_MAINSIGNALIN
                if (inputs[0] == 0){
                    inputs[0] = 1;
                }
                continue; // !
            } else if (!strcmp(flag, "-o")){
                outputs = parseBusses(argv, argc, "-o");
                continue; // !
            } else if (!strcmp(flag, "-k")){
                x_keep = true;
            } else if (!strcmp(flag, "-e")){
                editor = true;
            } else if (!strcmp(flag, "-sp")){
                precision = ProcessPrecision::Single;
            } else if (!strcmp(flag, "-dp")){
                precision = ProcessPrecision::Double;
            } else if (!strcmp(flag, "-s")){
                search = true;
            } else if (!strcmp(flag, "-t")){
                threaded = true;
            } else if (!strcmp(flag, "-p")){
                mode = RunMode::Sandbox;
            } else if (!strcmp(flag, "-b")){
                mode = RunMode::Bridge;
            } else {
                pd_error(this, "%s: unknown flag '%s'", classname(this), flag);
            }
            argc--; argv++;
        } else {
            file = argv->a_w.w_symbol;
            argc--; argv++;
            break;
        }
    }

    // inputs (default: 2), only if -i hasn't been used!
    if (inputs.empty()){
        // min. 1 because of CLASS_MAINSIGNALIN
        int in = argc > 0 ?
            std::max<int>(1, atom_getfloat(argv)) : 2;
        inputs.push_back(in);
    }
    LOG_DEBUG("inputs:");
    for (int i = 0; i < inputs.size(); ++i){
        LOG_DEBUG("  bus " << i << ": " << inputs[i] << "ch");
    }

    // outputs (default: 2), only if -o hasn't been used!
    if (outputs.empty()){
        int out = argc > 1 ?
            std::max<int>(0, atom_getfloat(argv + 1)) : 2;
        outputs.push_back(out);
    }
    LOG_DEBUG("outputs:");
    for (int i = 0; i < outputs.size(); ++i){
        LOG_DEBUG("  bus " << i << ": " << outputs[i] << "ch");
    }

    // (legacy) optional aux inputs/outputs
    // just add them to busses because they should not
    // be used together with the -i and -o flags
    int auxin = std::max<int>(0, atom_getfloatarg(2, argc, argv));
    if (auxin > 0){
        inputs.push_back(auxin);
    }
    int auxout = std::max<int>(0, atom_getfloatarg(3, argc, argv));
    if (auxout > 0){
        outputs.push_back(auxout);
    }

    x_wantprecision = precision;
    x_canvas = canvas_getcurrent();
    x_editor = std::make_unique<t_vsteditor>(*this, gui);
#ifdef PDINSTANCE
    x_pdinstance = pd_this;
#endif

    // inlets (we already have a main inlet!)
    int totalin = 0;
    for (auto& in : inputs){
        if (x_multi) {
            // one multi-channel inlet per bus
            totalin++;
        } else {
            totalin += in;
        }
        x_inlets.emplace_back(in);
    }
    // we already have a main inlet!
    while (--totalin){
        inlet_new(&x_obj, &x_obj.ob_pd, &s_signal, &s_signal);
    }
    // outlets:
    int totalout = 0;
    for (auto& out : outputs){
        if (x_multi) {
            // one multi-channel inlet per bus
            totalout++;
        } else {
            totalout += out;
        }
        x_outlets.emplace_back(out);
    }
    while (totalout--){
        outlet_new(&x_obj, &s_signal);
    }
    // additional message outlet
    x_messout = outlet_new(&x_obj, 0);

    if (search && !gDidSearch){
        for (auto& path : getDefaultSearchPaths()){
            // synchronous, parallel, no timeout
            searchPlugins<false>(path, nullptr);
        }
    #if 1
        writeCacheFile(); // shall we write cache file?
    #endif
        gDidSearch = true;
    }

    // needed in setup_plugin()!
    x_blocksize = sys_getblksize();
    x_sr = sys_getsr();

    // open plugin
    if (file){
        auto abspath = resolve_plugin_path(file->s_name);
        if (!abspath.empty()) {
            // for editor or plugin bridge/sandbox
            initEventLoop();

            t_open_data data;
            data.owner = this;
            data.pathsym = file;
            data.abspath = abspath;
            data.editor = editor;
            data.threaded = threaded;
            data.mode = mode;
            vstplugin_open_do<false>(&data);
            vstplugin_open_done(&data);
        } else {
            pd_error(this, "%s: cannot open '%s'", classname(this), file->s_name);
        }
        // HACK: always set path symbol so that in vstplugin_loadbang() we know that we
        // have to call vstplugin_open_notify() - even if the plugin has failed to load!
        x_path = file;
    }

    // restore state
    t_symbol *asym = gensym("#A");
    asym->s_thing = 0; // bashily unbind #A
    pd_bind(&x_obj.ob_pd, asym); // now bind #A to us to receive following messages

    // bind to global receive name
    pd_bind(&x_obj.ob_pd, gensym(glob_recv_name));
}

static void *vstplugin_new(t_symbol *s, int argc, t_atom *argv){
    auto x = pd_new(vstplugin_class);
    return new (x) t_vstplugin(argc, argv); // placement new
}

/*----------------------------- destructor ----------------------------*/

t_vstplugin::~t_vstplugin() {
    // we can stop the search without synchronizing with the worker thread!
    vstplugin_search_stop(this);

    // beforing closing the plugin, make sure that there are no pending tasks!
    // NOTE that this doesn't affect pending close commands because they can't
    // be issued while the plugin is suspended.
    if (x_suspended){
        t_workqueue::get()->cancel(this);
        x_suspended = false; // for vstplugin_close()!
    }

    if (x_plugin) {
        vstplugin_close(this);

        // Sync with UI thread if we're closing asynchronously,
        // see comment in vstplugin_close().
        if (x_async && x_uithread){
            UIThread::sync();
        }
    }

    LOG_DEBUG("vstplugin free");

    pd_unbind(&x_obj.ob_pd, gensym(glob_recv_name));

    t_workqueue::release();
}

static void vstplugin_free(t_vstplugin *x){
    x->~t_vstplugin();
}

/*-------------------------- perform routine ----------------------------*/

// TFloat: processing float type
// this templated method makes some optimization based on whether T and U are equal
template<typename TFloat>
static void vstplugin_doperform(t_vstplugin *x, int n){
    auto plugin = x->x_plugin.get();

    // first copy inlets into buffer
    // we have to do this even if the plugin uses the same float type
    // because inlets and outlets can alias!
    for (auto& inlets : x->x_inlets){
        for (int i = 0; i < inlets.b_n; ++i){
            auto src = inlets.b_signals[i];
            auto dst = (TFloat *)inlets.b_buffers[i];
            // NOTE: use a plain for-loop because we might need to
            // convert from t_sample to TFloat!
            for (int j = 0; j < n; ++j){
                dst[j] = src[j];
            }
        }
    }

    // process
    ProcessData data;
    data.numSamples = n;
    data.precision = x->x_realprecision;
    data.mode = x->x_mode;
    data.inputs = x->x_inputs.empty() ? nullptr : x->x_inputs.data();
    data.numInputs = x->x_inputs.size();
    data.outputs = x->x_outputs.empty() ? nullptr :  x->x_outputs.data();
    data.numOutputs = x->x_outputs.size();
    plugin->process(data);

    if (!std::is_same<t_sample, TFloat>::value){
        // copy output buffer to Pd outlets
        for (auto& outlets : x->x_outlets){
            for (int i = 0; i < outlets.b_n; ++i){
                auto src = (TFloat *)outlets.b_buffers[i];
                auto dst = outlets.b_signals[i];
                // NOTE: use a plain for-loop!
                for (int j = 0; j < n; ++j){
                    dst[j] = src[j];
                }
            }
        }
    }

    // zero remaining outlets
    int noutlets = x->x_outlets.size();
    int noutputs = x->x_outputs.size();
    if (noutlets == 1){
        // plugin outputs might be destributed
        auto& outlets = x->x_outlets[0];
        for (int i = x->x_output_channels; i < outlets.b_n; ++i){
            auto out = outlets.b_signals[i];
            std::fill(out, out + n, 0);
        }
    } else {
        for (int i = 0; i < noutlets; ++i){
            auto& outlets = x->x_outlets[i];
            int onset = (i < noutputs) ?
                        x->x_outputs[i].numChannels : 0;
            for (int j = onset; j < outlets.b_n; ++j){
                auto out = outlets.b_signals[j];
                std::fill(out, out + n, 0);
            }
        }
    }
}

static t_int *vstplugin_perform(t_int *w){
    t_vstplugin *x = (t_vstplugin *)(w[1]);
    int n = (int)(w[2]);
    x->x_lastdsptime = clock_getlogicaltime();

    // checking only x_process wouldn't be thread-safe!
    auto process = (x->x_plugin != nullptr) && x->x_process;
    // if async command is running, try to lock the mutex or bypass on failure
    std::unique_lock<SpinLock> lock;
    if (x->x_suspended) {
        // NB: we only need to lock if suspended!
        lock = std::unique_lock(x->x_mutex, std::try_to_lock);
        if (process && !lock) {
            LOG_DEBUG("vstplugin~: couldn't lock mutex");
            process = false;
        }
    }
    if (process){
        if (x->x_realprecision == ProcessPrecision::Double){
            vstplugin_doperform<double>(x, n);
        } else { // single precision
            vstplugin_doperform<float>(x, n);
        }
    } else {
        // bypass/zero
        // first copy all inlets into temporary buffer
        // because inlets and outlets can alias!
        for (auto& inlets : x->x_inlets){
            for (int i = 0; i < inlets.b_n; ++i){
                auto chn = inlets.b_signals[i];
                std::copy(chn, chn + n, (t_sample *)inlets.b_buffers[i]);
            }
        }
        // now copy inlets to corresponding outlets
        for (int i = 0; i < (int)x->x_outlets.size(); ++i){
            auto& outlets = x->x_outlets[i];
            if (i < (int)x->x_inlets.size()){
                auto& inlets = x->x_inlets[i];
                for (int j = 0; j < outlets.b_n; ++j){
                    if (j < inlets.b_n){
                        // copy buffer to outlet
                        auto chn = (const t_sample *)inlets.b_buffers[j];
                        std::copy(chn, chn + n, outlets.b_signals[j]);
                    } else {
                        // zero outlet
                        auto chn = outlets.b_signals[j];
                        std::fill(chn, chn + n, 0);
                    }
                }
            } else {
                // zero whole bus
                for (int j = 0; j < outlets.b_n; ++j){
                    auto chn = outlets.b_signals[j];
                    std::fill(chn, chn + n, 0);
                }
            }
        }
    }

    x->x_editor->flush_queues();

    return (w+3);
}

/*----------------------- loadbang method -------------------------*/

static void vstplugin_loadbang(t_vstplugin *x, t_floatarg action){
    // send message when plugin has been loaded (or failed to do so)
    // x_path is set in constructor
    if ((int)action == 0 && x->x_path) { // LB_LOAD
        vstplugin_open_notify(x);
        if (!x->x_plugin){
            x->x_path = nullptr; // undo HACK in vstplugin ctor
        }
    }
}

/*------------------------ save method ----------------------------*/

static void vstplugin_save(t_gobj *z, t_binbuf *bb){
    t_vstplugin *x = (t_vstplugin *)z;
    binbuf_addv(bb, "ssff", &s__X, gensym("obj"),
        (float)x->x_obj.te_xpix, (float)x->x_obj.te_ypix);
    binbuf_addbinbuf(bb, x->x_obj.ob_binbuf);
    binbuf_addsemi(bb);
    if (x->x_keep && x->x_plugin){
        // protect against concurrent vstplugin_open_do()
        t_scoped_nrt_lock lock(x->x_mutex);
        // 1) plugin
        binbuf_addv(bb, "ss", gensym("#A"), gensym("open"));
        if (x->x_uithread) {
            binbuf_addv(bb, "s", gensym("-e"));
        }
        if (x->x_threaded) {
            binbuf_addv(bb, "s", gensym("-t"));
        }
        if (x->x_runmode == RunMode::Bridge) {
            binbuf_addv(bb, "s", gensym("-b"));
        } else if (x->x_runmode == RunMode::Sandbox) {
            binbuf_addv(bb, "s", gensym("-p"));
        }
        binbuf_addv(bb, "s", x->x_path);
        binbuf_addsemi(bb);
        // 2) program number
        if (x->x_plugin->info().numPrograms() > 0) {
            binbuf_addv(bb, "ssi", gensym("#A"), gensym("program_set"), x->x_plugin->getProgram());
            binbuf_addsemi(bb);
        }
        // 3) program data
        std::string buffer;
        defer([&](){
            x->x_plugin->writeProgramData(buffer);
        }, x->x_uithread);
        int n = buffer.size();
        if (n){
            binbuf_addv(bb, "ss", gensym("#A"), gensym("program_data_set"));
            std::vector<t_atom> atoms;
            atoms.resize(n);
            for (int i = 0; i < n; ++i){
                    // first convert to range 0-255, then assign to t_float (not 100% portable...)
                SETFLOAT(&atoms[i], (unsigned char)buffer[i]);
            }
            binbuf_add(bb, n, atoms.data());
            binbuf_addsemi(bb);
        } else {
            pd_error(x, "%s: couldn't save program data", classname(x));
        }
    }
    obj_saveformat(&x->x_obj, bb);
}

/*------------------------ channels method -------------------------*/

static void vstplugin_channels(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv) {
    if (x->x_multi) {
        for (int i = 0; i < argc; ++i) {
            if (i < x->x_outlets.size()) {
                int nchans = atom_getfloat(argv + i);
                if (nchans >= 0) {
                    x->x_outlets[i].resize(nchans);
                } else {
                    pd_error(x, "%s: ignoring negative channel count for bus %d", classname(x), i);
                }
            } else {
                pd_error(x, "%s: output bus index %d out of range!", classname(x), i);
            }
        }
        // see "dsp" method!
        x->x_outchannels_changed = true;

        canvas_update_dsp();
    } else {
        pd_error(x, "%s: 'channels' message requires multi-channel mode", classname(x));
    }
}

/*-------------------------- dsp method ----------------------------*/

static void vstplugin_dsp(t_vstplugin *x, t_signal **sp){
    int oldblocksize = std::exchange(x->x_blocksize, sp[0]->s_n);
    int oldsr = std::exchange(x->x_sr, sp[0]->s_sr);
    int channels_changed = std::exchange(x->x_outchannels_changed, false);

    dsp_add(vstplugin_perform, 2, (t_int)x, (t_int)x->x_blocksize);

    // update signal vectors
    auto ptr = sp;
    // inlets
    if (x->x_multi) {
    #ifdef PD_HAVE_MULTICHANNEL
        // multi-channel mode
        for (auto& inlets : x->x_inlets) {
            int nchans = (*ptr)->s_nchans;
            // first check and update input channel count!
            if (nchans != inlets.b_n) {
                inlets.resize(nchans);
                channels_changed = true;
            }
            for (int i = 0; i < nchans; ++i) {
                inlets.b_signals[i] = (*ptr)->s_vec + i * x->x_blocksize;
            }
            ptr++;
        }
    #endif
    } else {
        // single-channel mode
        for (auto& inlets : x->x_inlets) {
            for (int i = 0; i < inlets.b_n; ++i, ++ptr){
                inlets.b_signals[i] = (*ptr)->s_vec;
            }
        }
    }
    // outlets
    for (auto& outlets : x->x_outlets){
        if (x->x_multi) {
        #ifdef PD_HAVE_MULTICHANNEL
            // multi-channel mode
            // NB: we must call this before accessing s_vec!
            g_signal_setmultiout(ptr, outlets.b_n);
            assert(outlets.b_n == (*ptr)->s_nchans);
            for (int i = 0; i < outlets.b_n; ++i){
                outlets.b_signals[i] = (*ptr)->s_vec + i * x->x_blocksize;
            }
            ptr++;
        #endif
        } else {
            // single-channel mode
            for (int i = 0; i < outlets.b_n; ++i, ++ptr){
                if (g_signal_setmultiout) {
                    // NB: we must call this before accessing s_vec!
                    g_signal_setmultiout(ptr, 1);
                }
                outlets.b_signals[i] = (*ptr)->s_vec;
            }
        }
    }

    // protect against concurrent vstplugin_open_do()
    t_scoped_nrt_lock lock(x->x_mutex);
    // only reset plugin if blocksize, samplerate or channels have changed
    if (x->x_plugin && ((x->x_blocksize != oldblocksize) || (x->x_sr != oldsr) || channels_changed)) {
        x->x_editor->defer_safe<false>([&](){
            x->setup_plugin(*x->x_plugin);
        }, x->x_uithread);
        if (x->x_threaded && (x->x_blocksize != oldblocksize)){
            // queue(!) latency change notification
            x->x_editor->latencyChanged(x->x_plugin->getLatencySamples());
        }
    }
    // always update buffers (also needed for bypassing!)
    x->update_buffers();
}

/*-------------------------- global methods ----------------------------*/

void vstplugin_dsp_threads(t_vstplugin *x, t_floatarg f) {
    int numthreads = f > 0 ? f : 0;
    setNumDSPThreads(numthreads);
}

/*-------------------------- private methods ---------------------------*/

void vstplugin_multichannel(t_vstplugin *x)
{
    t_atom a;
#ifdef PD_HAVE_MULTICHANNEL
    SETFLOAT(&a, g_signal_setmultiout != nullptr);
#else
    SETFLOAT(&a, -1); // compiled without multichannel support
#endif
    outlet_anything(x->x_messout, gensym("multichannel"), 1, &a);
}

/*-------------------------- setup function ----------------------------*/

#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#elif __GNUC__ >= 4
#define EXPORT extern "C" __attribute__((visibility("default")))
#else
#define EXPORT extern "C"
#endif

EXPORT void vstplugin_tilde_setup(void) {
#ifdef PD_HAVE_MULTICHANNEL
    // runtime check for multichannel support:
#ifdef _WIN32
    // get a handle to the module containing the Pd API functions.
    // NB: GetModuleHandle("pd.dll") does not cover all cases.
    HMODULE module;
    if (GetModuleHandleEx(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&pd_typedmess, &module)) {
        g_signal_setmultiout = (t_signal_setmultiout)(void *)GetProcAddress(
            module, "signal_setmultiout");
    }
#else
    // search recursively, starting from the main program
    g_signal_setmultiout = (t_signal_setmultiout)dlsym(
        dlopen(nullptr, RTLD_NOW), "signal_setmultiout");
#endif
#endif // PD_HAVE_MULTICHANNEL

    vstplugin_class = class_new(gensym("vstplugin~"), (t_newmethod)(void *)vstplugin_new,
        (t_method)vstplugin_free, sizeof(t_vstplugin), CLASS_MULTICHANNEL, A_GIMME, A_NULL);
    CLASS_MAINSIGNALIN(vstplugin_class, t_vstplugin, x_f);
    class_setsavefn(vstplugin_class, vstplugin_save);
    class_addmethod(vstplugin_class, (t_method)vstplugin_dsp, gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_loadbang, gensym("loadbang"), A_FLOAT, A_NULL);
    if (g_signal_setmultiout) {
        class_addmethod(vstplugin_class, (t_method)vstplugin_channels, gensym("channels"), A_GIMME, A_NULL);
    }
    // plugin
    class_addmethod(vstplugin_class, (t_method)vstplugin_open, gensym("open"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_close, gensym("close"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_search, gensym("search"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_search_stop, gensym("search_stop"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_cache_clear, gensym("search_clear"), A_DEFFLOAT, A_NULL); // deprecated
    class_addmethod(vstplugin_class, (t_method)vstplugin_cache_clear, gensym("cache_clear"), A_DEFFLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_cache_read, gensym("cache_read"), A_DEFSYM, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_plugin_list, gensym("plugin_list"), A_NULL);

    class_addmethod(vstplugin_class, (t_method)vstplugin_bypass, gensym("bypass"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_reset, gensym("reset"), A_DEFFLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_offline, gensym("offline"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_vis, gensym("vis"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_pos, gensym("pos"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_size, gensym("size"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_click, gensym("click"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_info, gensym("info"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_can_do, gensym("can_do"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_vendor_method, gensym("vendor_method"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_print, gensym("print"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_version, gensym("version"), A_NULL);
    // transport
    class_addmethod(vstplugin_class, (t_method)vstplugin_tempo, gensym("tempo"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_time_signature, gensym("time_signature"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_play, gensym("play"), A_FLOAT, A_NULL);
#if 0
    class_addmethod(vstplugin_class, (t_method)vstplugin_cycle, gensym("cycle"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_cycle_start, gensym("cycle_start"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_cycle_end, gensym("cycle_end"), A_FLOAT, A_NULL);
#endif
    class_addmethod(vstplugin_class, (t_method)vstplugin_transport_set, gensym("transport_set"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_transport_get, gensym("transport_get"), A_NULL);
    // inputs/outputs
    class_addmethod(vstplugin_class, (t_method)vstplugin_bus_info<t_direction::in>, gensym("input_info"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_bus_list<t_direction::in>, gensym("input_list"), A_DEFSYM, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_input_count, gensym("input_count"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_bus_info<t_direction::out>, gensym("output_info"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_bus_list<t_direction::out>, gensym("output_list"), A_DEFSYM, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_output_count, gensym("output_count"), A_NULL);
    // parameters
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_set, gensym("param_set"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_get, gensym("param_get"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_info, gensym("param_info"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_count, gensym("param_count"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_list, gensym("param_list"), A_DEFSYM, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_dump, gensym("param_dump"), A_NULL);
    // midi
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_raw, gensym("midi_raw"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_note, gensym("midi_note"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_noteoff, gensym("midi_noteoff"), A_FLOAT, A_FLOAT, A_DEFFLOAT, A_NULL); // third floatarg is optional!
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_cc, gensym("midi_cc"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_bend, gensym("midi_bend"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_program, gensym("midi_program"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_polytouch, gensym("midi_polytouch"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_touch, gensym("midi_touch"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_sysex, gensym("midi_sysex"), A_GIMME, A_NULL);
    // programs
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_set, gensym("program_set"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_get, gensym("program_get"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_name_set, gensym("program_name_set"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_name_get, gensym("program_name_get"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_count, gensym("program_count"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_list, gensym("program_list"), A_DEFSYM, A_NULL);
    // presets
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_count, gensym("preset_count"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_info, gensym("preset_info"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_list, gensym("preset_list"), A_DEFSYM, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_load, gensym("preset_load"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_save, gensym("preset_save"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_rename, gensym("preset_rename"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_delete, gensym("preset_delete"), A_GIMME, A_NULL);
    // read/write fx programs
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_data_set<PROGRAM>, gensym("program_data_set"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_data_get<PROGRAM>, gensym("program_data_get"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_read<PROGRAM>, gensym("program_read"), A_SYMBOL, A_DEFFLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_write<PROGRAM>, gensym("program_write"), A_SYMBOL, A_DEFFLOAT, A_NULL);
    // read/write fx banks
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_data_set<BANK>, gensym("bank_data_set"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_data_get<BANK>, gensym("bank_data_get"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_read<BANK>, gensym("bank_read"), A_SYMBOL, A_DEFFLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_write<BANK>, gensym("bank_write"), A_SYMBOL, A_DEFFLOAT, A_NULL);
    // global messages
    class_addmethod(vstplugin_class, (t_method)vstplugin_dsp_threads, gensym("dsp_threads"), A_DEFFLOAT, A_NULL);
    // private messages
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_change, gensym("preset_change"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_multichannel, gensym("multichannel"), A_NULL);

    vstparam_setup();

    workqueue_setup();

    // NOTE: at the time of writing (Pd 0.54), the class free function is not actually called;
    // hopefully, this will change in future Pd versions.
    class_setfreefn(vstplugin_class, [](t_class *) {
        // This makes sure that all plugin factories are released here and not
        // in the global object destructor (which can cause crashes or deadlocks!)
        gPluginDict.clear();

        workqueue_terminate();
    });

    post("vstplugin~ %s", getVersionString());

    // read cached plugin info
    readCacheFile();
}
