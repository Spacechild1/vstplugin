#include "vstplugin~.h"

#undef pd_class
#define pd_class(x) (*(t_pd *)(x))
#define classname(x) (class_getname(pd_class(x)))

#if !HAVE_UI_THREAD // don't use VST GUI threads
# define EVENT_LOOP_POLL_INT 20 // time between polls in ms
static t_clock *eventLoopClock = nullptr;
static void eventLoopTick(void *x){
    UIThread::poll();
    clock_delay(eventLoopClock, EVENT_LOOP_POLL_INT);
}
#endif

// substitute SPACE for NO-BREAK SPACE (e.g. to avoid Tcl errors in the properties dialog)
static void substitute_whitespace(char *buf){
    for (char *c = buf; *c; c++){
        if (*c == ' ') *c = 160;
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
        u = std::stoull(s, 0, 0);
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

/*---------------------- search/probe ----------------------------*/

static PluginManager gPluginManager;

#define SETTINGS_DIR ".vstplugin~"
// so that 64-bit and 32-bit installations can co-exist!
#if (defined(_WIN32) && !defined(_WIN64)) || defined(__i386__)
#define SETTINGS_FILE "cache32.ini"
#else
#define SETTINGS_FILE "cache.ini"
#endif

static std::string getSettingsDir(){
#ifdef _WIN32
    return expandPath("%USERPROFILE%\\" SETTINGS_DIR);
#else
    return expandPath("~/" SETTINGS_DIR);
#endif
}

static std::mutex gFileMutex;

static void readIniFile(){
    std::lock_guard<std::mutex> lock(gFileMutex);
    try {
        gPluginManager.read(getSettingsDir() + "/" SETTINGS_FILE);
    } catch (const Error& e){
        error("couldn't read settings file:");
        error("%s", e.what());
    }
}

static void writeIniFile(){
    std::lock_guard<std::mutex> lock(gFileMutex);
    try {
        auto dir = getSettingsDir();
        if (!pathExists(dir)){
            if (!createDirectory(dir)){
                throw Error("couldn't create directory");
            }
        }
        gPluginManager.write(dir + "/" SETTINGS_FILE);
    } catch (const Error& e){
        error("couldn't write settings file:");
        error("%s", e.what());
    }
}

// for asynchronous searching, we want to show the name of the plugin before
// the result, especially if the plugin takes a long time to load (e.g. shell plugins).
// The drawback is that we either have to post the result on a seperate line or post
// on the normal log level. For now, we do the latter.
// NOTE: when probing plugins in parallel, we obviously can't do this, we have to
// show the name and result at the same time. This is shouldn't be much of a problem
// as there will be more activity.
template<bool async = false>
class PdLog {
public:
    template <typename... T>
    PdLog(PdLogLevel level, const char *fmt, T... args)
        : PdLog(level)
    {
        if (async){
            // post immediately
            sys_lock();
            if (level >= PD_NORMAL){
                startpost(fmt, args...);
                force_ = true; // force newline on destruction!
            } else {
                verbose(level, fmt, args...);
            }
            sys_unlock();
        } else {
            // defer posting
            char buf[MAXPDSTRING];
            snprintf(buf, MAXPDSTRING, fmt, args...);
            ss_ << buf;
        }
    }
    PdLog(PdLogLevel level)
        : level_(level){}
    PdLog(PdLog&& other)
        : ss_(std::move(other.ss_)), level_(other.level_)
    {
        force_ = other.force_;
        other.force_ = false; // make sure that moved from object doesn't print
    }
    ~PdLog(){
        flush();
    }
    PdLog& flush(){
        auto str = ss_.str();
        if (!str.empty()){
            if (async){
                sys_lock();
                post("%s", ss_.str().c_str());
                sys_unlock();
            } else {
                verbose(level_, "%s", ss_.str().c_str());
            }
            ss_ = std::stringstream(); // reset stream
        } else if (force_){
            endpost();
        }
        return *this;
    }
    PdLog& operator <<(const std::string& s){
        ss_ << s;
        return *this;
    }
    PdLog& operator <<(const Error& e){
        flush();
        if (async){
            sys_lock();
            verbose(PD_ERROR, "%s", e.what());
            sys_unlock();
        } else {
            verbose(PD_ERROR, "%s", e.what());
        }
        return *this;
    }
    PdLog& operator <<(const ProbeResult& result){
        switch (result.error.code()){
        case Error::NoError:
            *this << "ok!";
            break;
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
        return *this;
    }
private:
    std::stringstream ss_;
    PdLogLevel level_;
    bool force_ = false;
};

template<typename T>
void consume(T&& obj){
    T dummy(std::move(obj));
}

template<bool async = false, typename... T>
void postBug(const char *fmt, T... args){
    if (async) sys_lock();
    bug(fmt, args...);
    if (async) sys_unlock();
}

template<bool async = false, typename... T>
void postError(const char *fmt, T... args){
    if (async) sys_lock();
    error(fmt, args...);
    if (async) sys_unlock();
}


// load factory and probe plugins
template<bool async>
static IFactory::ptr loadFactory(const std::string& path){
    IFactory::ptr factory;

    if (gPluginManager.findFactory(path)){
        postBug<async>("loadFactory");
        return nullptr;
    }
    if (gPluginManager.isException(path)){
        PdLog<async> log(PD_DEBUG, "'%s' is black-listed", path.c_str());
        return nullptr;
    }

    try {
        factory = IFactory::load(path);
    } catch (const Error& e){
        PdLog<async> log(PD_ERROR, "couldn't load '%s': %s", path.c_str(), e.what());
        gPluginManager.addException(path);
        return nullptr;
    }

    return factory;
}

// VST2: plug-in name
// VST3: plug-in name + ".vst3"
static std::string makeKey(const PluginInfo& desc){
    std::string key;
    auto ext = ".vst3";
    auto onset = std::max<size_t>(0, desc.path.size() - strlen(ext));
    if (desc.path.find(ext, onset) != std::string::npos){
        key = desc.name + ext;
    } else {
        key = desc.name;
    }
    return key;
}

static bool addFactory(const std::string& path, IFactory::ptr factory){
    if (factory->numPlugins() == 1){
        auto plugin = factory->getPlugin(0);
        // factories with a single plugin can also be aliased by their file path(s)
        gPluginManager.addPlugin(plugin->path, plugin);
        gPluginManager.addPlugin(path, plugin);
    }

    if (factory->valid()){
        gPluginManager.addFactory(path, factory);
        for (int i = 0; i < factory->numPlugins(); ++i){
            auto plugin = factory->getPlugin(i);
            // also map bashed parameter names
            int num = plugin->parameters.size();
            for (int j = 0; j < num; ++j){
                auto key = plugin->parameters[j].name;
                bash_name(key);
                const_cast<PluginInfo&>(*plugin).addParamAlias(j, key);
            }
            // search for presets
            const_cast<PluginInfo&>(*plugin).scanPresets();
            // add plugin info
            auto key = makeKey(*plugin);
            gPluginManager.addPlugin(key, plugin);
            bash_name(key); // also add bashed version!
            gPluginManager.addPlugin(key, plugin);
        }
        return true;
    } else {
        gPluginManager.addException(path);
        return false;
    }
}

template<bool async>
static IFactory::ptr probePlugin(const std::string& path){
    auto factory = loadFactory<async>(path);
    if (!factory){
        return nullptr;
    }

    PdLog<async> log(PD_DEBUG, "probing '%s'... ", path.c_str());

    try {
        factory->probe([&](const ProbeResult& result){
            if (result.total > 1){
                if (result.index == 0){
                    consume(std::move(log)); // force
                }
                // Pd's posting methods have a size limit, so we log each plugin seperately!
                PdLog<async> log1(PD_DEBUG, "\t[%d/%d] ", result.index + 1, result.total);
                if (result.plugin && !result.plugin->name.empty()){
                    log1 << "'" << result.plugin->name << "' ";
                }
                log1 << "... " << result;
            } else {
                log << result;
                consume(std::move(log));
            }
        });
        if (addFactory(path, factory)){
            return factory; // success
        }
    } catch (const Error& e){
        ProbeResult result;
        result.error = e;
        log << e;
    }
    return nullptr;
}

using FactoryFuture = std::function<IFactory::ptr()>;

template<bool async>
static FactoryFuture probePluginParallel(const std::string& path){
    auto factory = loadFactory<async>(path);
    if (!factory){
        return []() { return nullptr;  };
    }
    try {
        // start probing process
        auto future = factory->probeAsync();
        // return future
        return [=]() -> IFactory::ptr {
            PdLog<async> log(PD_DEBUG, "probing '%s'... ", path.c_str());
            // wait for results
            future([&](const ProbeResult& result){
                if (result.total > 1){
                    if (result.index == 0){
                        consume(std::move(log)); // force
                    }
                    // Pd's posting methods have a size limit, so we log each plugin seperately!
                    PdLog<async> log1(PD_DEBUG, "\t[%d/%d] ", result.index + 1, result.total);
                    if (result.plugin && !result.plugin->name.empty()){
                        log1 << "'" << result.plugin->name << "' ";
                    }
                    log1 << "... " << result;
                } else {
                    log << result;
                    consume(std::move(log));
                }
            }); // collect result(s)
            if (addFactory(path, factory)){
                return factory;
            } else {
                return nullptr;
            }
        };
    } catch (const Error& e){
        // return future which prints the error message
        return [=]() -> IFactory::ptr {
            PdLog<async> log(PD_DEBUG, "probing '%s'... ", path.c_str());
            ProbeResult result;
            result.error = e;
            log << result;
            return nullptr;
        };
    }
}

#define PROBE_PROCESSES 8

template<bool async>
static void searchPlugins(const std::string& path, bool parallel, t_vstplugin::t_search_data_ptr data = nullptr){
    int count = 0;
    {
        std::string bashPath = path;
        sys_unbashfilename(&bashPath[0], &bashPath[0]);
        PdLog<async> log(PD_NORMAL, "searching in '%s' ...", bashPath.c_str()); // destroy
    }

    auto addPlugin = [&](const PluginInfo& plugin, int which = 0, int n = 0){
        if (data){
            auto key = makeKey(plugin);
            bash_name(key);
            data->s_plugins.push_back(gensym(key.c_str()));
        }
        // Pd's posting methods have a size limit, so we log each plugin seperately!
        if (n > 0){
            PdLog<async> log(PD_DEBUG, "\t[%d/%d] ", which + 1, n);
            log << plugin.name;
        }
        count++;
    };

    std::vector<FactoryFuture> futures;

    auto processFutures = [&](){
        for (auto& f : futures){
            auto factory = f();
            if (factory){
                int numPlugins = factory->numPlugins();
                for (int i = 0; i < numPlugins; ++i){
                    addPlugin(*factory->getPlugin(i));
                }
            }
        }
        futures.clear();
    };

    vst::search(path, [&](const std::string& absPath){
        if (data && !data->s_running){
            return; // search was cancelled
        }
        LOG_DEBUG("found " << absPath);
        std::string pluginPath = absPath;
        sys_unbashfilename(&pluginPath[0], &pluginPath[0]);
        // check if module has already been loaded
        auto factory = gPluginManager.findFactory(pluginPath);
        if (factory){
            // just post paths of valid plugins
            PdLog<async> log(PD_DEBUG, "%s", factory->path().c_str());
            auto numPlugins = factory->numPlugins();
            if (numPlugins == 1){
                addPlugin(*factory->getPlugin(0));
            } else {
                consume(std::move(log)); // force
                for (int i = 0; i < numPlugins; ++i){
                    addPlugin(*factory->getPlugin(i), i, numPlugins);
                }
            }
        } else {
            // probe (will post results and add plugins)
            if (parallel){
                futures.push_back(probePluginParallel<async>(pluginPath));
                if (futures.size() >= PROBE_PROCESSES){
                    processFutures();
                }
            } else {
                if ((factory = probePlugin<async>(pluginPath))){
                    int numPlugins = factory->numPlugins();
                    for (int i = 0; i < numPlugins; ++i){
                        addPlugin(*factory->getPlugin(i));
                    }
                }
            }
        }
    });
    processFutures();

    if (count == 1){
        PdLog<async> log(PD_NORMAL, "found 1 plugin");
    } else {
        PdLog<async> log(PD_NORMAL, "found %d plugins", count);
    }
}

// tell whether we've already searched the standard VST directory
// (see '-s' flag for [vstplugin~])
static bool gDidSearch = false;

/*--------------------- t_vstparam --------------------------*/

static t_class *vstparam_class;

t_vstparam::t_vstparam(t_vstplugin *x, int index)
    : p_owner(x), p_index(index){
    p_pd = vstparam_class;
    char buf[64];
        // slider
    snprintf(buf, sizeof(buf), "%p-hsl-%d", x, index);
    p_slider = gensym(buf);
    pd_bind(&p_pd, p_slider);
        // display
    snprintf(buf, sizeof(buf), "%p-d-%d-snd", x, index);
    p_display_snd = gensym(buf);
    pd_bind(&p_pd, p_display_snd);
    snprintf(buf, sizeof(buf), "%p-d-%d-rcv", x, index);
    p_display_rcv = gensym(buf);
}

t_vstparam::~t_vstparam(){
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
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", plugin.getParameterString(index).c_str());
    pd_vmess(x->p_display_rcv->s_thing, gensym("set"), (char *)"s", gensym(buf));
}

static void vstparam_setup(){
    vstparam_class = class_new(gensym("__vstparam"), 0, 0, sizeof(t_vstparam), 0, A_NULL);
    class_addfloat(vstparam_class, (t_method)vstparam_float);
    class_addsymbol(vstparam_class, (t_method)vstparam_symbol);
    class_addmethod(vstparam_class, (t_method)vstparam_set, gensym("set"), A_DEFFLOAT, 0);
}

/*-------------------- t_vsteditor ------------------------*/

t_vsteditor::t_vsteditor(t_vstplugin &owner, bool gui)
    : e_owner(&owner){
#if HAVE_UI_THREAD
    e_mainthread = std::this_thread::get_id();
#endif
    if (gui){
        pd_vmess(&pd_canvasmaker, gensym("canvas"), (char *)"iiiii", 0, 0, 100, 100, 10);
        e_canvas = (t_canvas *)s__X.s_thing;
        send_vmess(gensym("pop"), "i", 0);
    }
    e_clock = clock_new(this, (t_method)tick);
}

t_vsteditor::~t_vsteditor(){
    clock_free(e_clock);
}

// post outgoing event (thread-safe if needed)
template<typename T, typename U>
void t_vsteditor::post_event(T& queue, U&& event){
#if HAVE_UI_THREAD
    bool mainthread = std::this_thread::get_id() == e_mainthread;
#else
    bool mainthread = true;
#endif
    // prevent event scheduling from within the tick method to avoid
    // deadlocks or memory errors
    if (mainthread && e_tick){
        pd_error(e_owner, "%s: recursion detected", classname(e_owner));
        return;
    }
#if HAVE_UI_THREAD
    // we only need to lock for VST GUI editors
    bool editor = window();
    if (editor){
        e_mutex.lock();
    }
#endif
    queue.push_back(std::forward<U>(event));
#if HAVE_UI_THREAD
    if (editor){
        e_mutex.unlock();
    }

    if (mainthread){
        clock_delay(e_clock, 0);
    } else {
        // Only lock Pd if DSP is off. This is better for real-time safety and it also
        // prevents a possible deadlock with plugins that use a mutex for synchronization
        // between UI thread and processing thread.
        // Calling pd_getdspstate() is not really thread-safe, though...
        if (pd_getdspstate()){
            e_needclock.store(true); // set the clock in the perform routine
        } else {
            // lock the Pd scheduler
            sys_lock();
            clock_delay(e_clock, 0);
            sys_unlock();
        }
    }
#else
    clock_delay(e_clock, 0);
#endif
}

// parameter automation notification might come from another thread (VST GUI editor).
void t_vsteditor::parameterAutomated(int index, float value){
    post_event(e_automated, std::make_pair(index, value));
}

// MIDI and SysEx events might be send from both the audio thread (e.g. arpeggiator) or GUI thread (MIDI controller)
void t_vsteditor::midiEvent(const MidiEvent &event){
    post_event(e_midi, event);
}

void t_vsteditor::sysexEvent(const SysexEvent &event){
    post_event(e_sysex, event);
}

void t_vsteditor::tick(t_vsteditor *x){
    t_outlet *outlet = x->e_owner->x_messout;
    x->e_tick = true; // prevent recursion

#if HAVE_UI_THREAD
    // we only need to lock for VST GUI editors
    bool editor = x->window();
    if (editor){
        // it's more important not to block than flushing the queues on time
        if (!x->e_mutex.try_lock()){
            LOG_DEBUG("couldn't lock mutex");
            x->e_tick = false;
            return;
        }
    }
#endif
    // automated parameters:
    for (auto& param : x->e_automated){
        int index = param.first;
        float value = param.second;
        // update the generic GUI
        x->param_changed(index, value);
        // send message
        t_atom msg[2];
        SETFLOAT(&msg[0], index);
        SETFLOAT(&msg[1], value);
        outlet_anything(outlet, gensym("param_automated"), 2, msg);
    }
    x->e_automated.clear();
    // midi events:
    for (auto& midi : x->e_midi){
        t_atom msg[3];
        SETFLOAT(&msg[0], (unsigned char)midi.data[0]);
        SETFLOAT(&msg[1], (unsigned char)midi.data[1]);
        SETFLOAT(&msg[2], (unsigned char)midi.data[2]);
        outlet_anything(outlet, gensym("midi"), 3, msg);
    }
    x->e_midi.clear();
    // sysex events:
    for (auto& sysex : x->e_sysex){
        std::vector<t_atom> msg;
        int n = sysex.size;
        msg.resize(n);
        for (int i = 0; i < n; ++i){
            SETFLOAT(&msg[i], (unsigned char)sysex.data[i]);
        }
        outlet_anything(outlet, gensym("midi"), n, msg.data());
    }
    x->e_sysex.clear();
#if HAVE_UI_THREAD
    if (editor){
        x->e_mutex.unlock();
    }
#endif
    x->e_tick = false;
}

const int xoffset = 30;
const int yoffset = 30;
const int maxparams = 16; // max. number of params per column
const int row_width = 128 + 10 + 128; // slider + symbol atom + label
const int col_height = 40;

void t_vsteditor::setup(){
    if (!pd_gui()){
        return;
    }
    auto& info = e_owner->x_plugin->info();

    send_vmess(gensym("rename"), (char *)"s", gensym(info.name.c_str()));
    send_mess(gensym("clear"));

    int nparams = info.numParameters();
    e_params.clear();
    // reserve to avoid a reallocation (which will call destructors)
    e_params.reserve(nparams);
    for (int i = 0; i < nparams; ++i){
        e_params.emplace_back(e_owner, i);
    }
        // slider: #X obj ...
    char sliderText[] = "25 43 hsl 128 15 0 1 0 0 snd rcv label -2 -8 0 10 -262144 -1 -1 0 1";
    t_binbuf *sliderBuf = binbuf_new();
    binbuf_text(sliderBuf, sliderText, strlen(sliderText));
    t_atom *slider = binbuf_getvec(sliderBuf);
        // display: #X symbolatom ...
    char displayText[] = "165 79 10 0 0 1 label rcv snd";
    t_binbuf *displayBuf = binbuf_new();
    binbuf_text(displayBuf, displayText, strlen(displayText));
    t_atom *display = binbuf_getvec(displayBuf);

    int ncolumns = nparams / maxparams + ((nparams % maxparams) != 0);
    if (!ncolumns) ncolumns = 1; // just to prevent division by zero
    int nrows = nparams / ncolumns + ((nparams % ncolumns) != 0);

    for (int i = 0; i < nparams; ++i){
        int col = i / nrows;
        int row = i % nrows;
        int xpos = xoffset + col * row_width;
        int ypos = yoffset + row * col_height;
            // create slider
        SETFLOAT(slider, xpos);
        SETFLOAT(slider+1, ypos);
        SETSYMBOL(slider+9, e_params[i].p_slider);
        SETSYMBOL(slider+10, e_params[i].p_slider);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d: %s", i, info.parameters[i].name.c_str());
        substitute_whitespace(buf);
        SETSYMBOL(slider+11, gensym(buf));
        send_mess(gensym("obj"), 21, slider);
            // create display
        SETFLOAT(display, xpos + 128 + 10); // slider + space
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

    update();

    binbuf_free(sliderBuf);
    binbuf_free(displayBuf);
}

void t_vsteditor::update(){
    if (!e_owner->check_plugin()) return;
    if (window()){
        window()->update();
    } else if (e_canvas) {
        int n = e_owner->x_plugin->info().numParameters();
        for (int i = 0; i < n; ++i){
            param_changed(i, e_owner->x_plugin->getParameter(i));
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

void t_vsteditor::flush_queues(){
#if HAVE_UI_THREAD
    bool expected = true;
    if (e_needclock.compare_exchange_strong(expected, false)){
        clock_delay(e_clock, 0);
    }
#endif
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

/*---------------- t_vstplugin (public methods) ------------------*/

// search
static void vstplugin_search_done(t_vstplugin *x){
    verbose(PD_NORMAL, "search done");
    // sort plugin names alphabetically and case independent
    auto& plugins = x->x_search_data->s_plugins;
    std::sort(plugins.begin(), plugins.end(), [](const auto& lhs, const auto& rhs){
        std::string s1 = lhs->s_name;
        std::string s2 = rhs->s_name;
        for (auto& c : s1) { c = std::tolower(c); }
        for (auto& c : s2) { c = std::tolower(c); }
        return s1 < s2;
    });
    for (auto& plugin : plugins){
        t_atom msg;
        SETSYMBOL(&msg, plugin);
        outlet_anything(x->x_messout, gensym("plugin"), 1, &msg);
    }
    outlet_anything(x->x_messout, gensym("search_done"), 0, nullptr);
    x->x_search_data->s_running = false;
}

static void vstplugin_search_threadfun(t_vstplugin *x, t_vstplugin::t_search_data_ptr data,
                                       std::vector<std::string> searchPaths,
                                       bool parallel, bool update){
    LOG_DEBUG("thread function started: " << std::this_thread::get_id());
    for (auto& path : searchPaths){
        if (data->s_running){
            searchPlugins<true>(path, parallel, data); // async
        } else {
            break;
        }
    }

    // we need to lock Pd before calling clock_delay().
    // also, 's_running' won't change its value while we hold the lock.
    sys_lock();
    if (data->s_running){
        clock_delay(x->x_search_clock, 0); // schedules vstplugin_search_done
    } else {
        LOG_DEBUG("search cancelled!");
        update = false; // don't update cache file
    }
    sys_unlock();

    if (update){
        writeIniFile(); // mutex protected
    }
    LOG_DEBUG("thread function terminated");
}

static void vstplugin_search(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    bool async = false;
    bool parallel = true; // for now, always do a parallel search
    bool update = true; // update cache file
    std::vector<std::string> searchPaths;

    if (x->x_search_data->s_running){
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
            } else {
                pd_error(x, "%s: unknown flag '%s'", classname(x), flag);
            }
            argv++; argc--;
        } else {
            break;
        }
    }

    x->x_search_data->s_plugins.clear(); // clear list of plug-in keys
    x->x_search_data->s_running = true;

    if (argc > 0){
        while (argc--){
            auto sym = atom_getsymbol(argv++);
            char path[MAXPDSTRING];
            canvas_makefilename(x->x_canvas, sym->s_name, path, MAXPDSTRING);
            if (async){
                searchPaths.emplace_back(path); // save for later
            } else {
                searchPlugins<false>(path, parallel, x->x_search_data);
            }
        }
    } else {
        // search in the default VST search paths if no user paths were provided
        for (auto& path : getDefaultSearchPaths()){
            if (async){
                searchPaths.emplace_back(path); // save for later
            } else {
                searchPlugins<false>(path, parallel, x->x_search_data);
            }
        }
    }

    if (async){
        // spawn thread which does the actual searching in the background
        auto thread = std::thread(vstplugin_search_threadfun, x, x->x_search_data,
                                  std::move(searchPaths), parallel, update);
        thread.detach();
    } else {
        if (update){
            writeIniFile();
        }
        vstplugin_search_done(x);
    }
}

static void vstplugin_search_stop(t_vstplugin *x){
    x->x_search_data->s_running = false;
}

static void vstplugin_search_clear(t_vstplugin *x, t_floatarg f){
        // unloading plugins might crash, so we we first delete the cache file
    if (f != 0){
        removeFile(getSettingsDir() + "/" SETTINGS_FILE);
    }
        // clear the plugin description dictionary
    gPluginManager.clear();
}

// resolves relative paths to an existing plugin in the canvas search paths or VST search paths.
// returns empty string on failure!
static std::string resolvePath(t_canvas *c, const std::string& s){
    std::string result;
        // resolve relative path
    if (!sys_isabsolutepath(s.c_str())){
        bool vst3 = false;
        std::string path = s;
    #ifdef _WIN32
        const char *ext = ".dll";
    #elif defined(__APPLE__)
        const char *ext = ".vst";
    #else // Linux/BSD/etc.
        const char *ext = ".so";
    #endif
        if (path.find(".vst3") != std::string::npos){
            vst3 = true;
        } else if (path.find(ext) == std::string::npos){
            path += ext;
        }
            // first try canvas search paths
        char fullPath[MAXPDSTRING];
        char dirresult[MAXPDSTRING];
        char *name = nullptr;
    #ifdef __APPLE__
        const char *bundlePath = "Contents/Info.plist";
        // on MacOS VST plugins are always bundles (directories) but canvas_open needs a real file
        snprintf(fullPath, MAXPDSTRING, "%s/%s", path.c_str(), bundlePath);
        int fd = canvas_open(c, fullPath, "", dirresult, &name, MAXPDSTRING, 1);
    #else
        const char *bundlePath = nullptr;
        int fd = canvas_open(c, path.c_str(), "", dirresult, &name, MAXPDSTRING, 1);
        if (fd < 0 && vst3){
            // VST3 plugins might be bundles
            bundlePath = getBundleBinaryPath().c_str();
        #ifdef _WIN32
            snprintf(fullPath, MAXPDSTRING, "%s/%s/%s",
                     path.c_str(), bundlePath, fileName(path).c_str());
        #else
            snprintf(fullPath, MAXPDSTRING, "%s/%s/%s.so",
                     path.c_str(), bundlePath, fileBaseName(path).c_str());
         #endif
            fd = canvas_open(c, fullPath, "", dirresult, &name, MAXPDSTRING, 1);
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
            result = buf; // success
        } else {
                // otherwise try default VST paths
            for (auto& vstpath : getDefaultSearchPaths()){
                result = vst::find(vstpath, path);
                if (!result.empty()) break; // success
            }
        }
    } else {
        result = s;
    }
    sys_unbashfilename(&result[0], &result[0]);
    return result;
}

// query a plugin by its key or file path and probe if necessary.
static const PluginInfo * queryPlugin(t_vstplugin *x, const std::string& path){
    // query plugin
    auto desc = gPluginManager.findPlugin(path);
    if (!desc){
            // try as file path
        std::string abspath = resolvePath(x->x_canvas, path);
        if (abspath.empty()){
            verbose(PD_DEBUG, "'%s' is neither an existing plugin name "
                    "nor a valid file path", path.c_str());
        } else if (!(desc = gPluginManager.findPlugin(abspath))){
                // finally probe plugin
            if (probePlugin<false>(abspath)){
                desc = gPluginManager.findPlugin(abspath);
                // findPlugin() fails if the module contains several plugins,
                // which means the path can't be used as a key.
                if (!desc){
                    verbose(PD_DEBUG, "'%s' contains more than one plugin. "
                            "Please use the 'search' method and open the desired "
                            "plugin by its name.", abspath.c_str());
                }
            }
        }
   }
   return desc.get();
}

// close
static void vstplugin_close(t_vstplugin *x){
    if (x->x_plugin){
        if (x->x_uithread){
            try {
                UIThread::destroy(std::move(x->x_plugin));
            } catch (const Error& e){
                pd_error(x, "%s: couldn't close plugin: %s",
                         classname(x), e.what());
            }
        }
        x->x_plugin = nullptr;
        x->x_editor->vis(false);
        x->x_key = nullptr;
        x->x_path = nullptr;
        x->x_preset = nullptr;
    }
}

// open
static void vstplugin_open(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    t_symbol *pathsym = nullptr;
    bool editor = false;
        // parse arguments
    while (argc && argv->a_type == A_SYMBOL){
        auto sym = argv->a_w.w_symbol;
        if (*sym->s_name == '-'){ // flag
            const char *flag = sym->s_name;
            if (!strcmp(flag, "-e")){
                editor = true;
            } else {
                pd_error(x, "%s: unknown flag '%s'", classname(x), flag);
            }
            argc--; argv++;
        } else { // file name
            pathsym = sym;
            break;
        }
    }
    // don't reopen the same plugin (mainly for -k flag)
    if (pathsym == x->x_path && x->x_editor->vst_gui() == editor){
        return;
    }
    bool success = x->open_plugin(pathsym, editor);
    // output message
    t_atom a;
    SETFLOAT(&a, success);
    outlet_anything(x->x_messout, gensym("open"), 1, &a);
}

bool t_vstplugin::open_plugin(t_symbol *s, bool editor){
    // close the old plugin
    vstplugin_close(this);

    if (!s){
        pd_error(this, "%s: 'open' needs a symbol argument!", classname(this));
        return false;
    }
    // get plugin info
    const PluginInfo *info = queryPlugin(this, s->s_name);
    if (!info){
        pd_error(this, "%s: can't load '%s'", classname(this), s->s_name);
        return false;
    }
    // open the new VST plugin
    try {
        IPlugin::ptr plugin;
        if (editor){
            plugin = UIThread::create(*info);
        } else {
            plugin = info->create();
        }
        x_uithread = editor;
        x_path = s; // store path symbol (to avoid reopening the same plugin)
        x_key = gensym(makeKey(*info).c_str());
        verbose(PD_DEBUG, "opened '%s'", info->name.c_str());
        // setup plugin
        x_plugin = std::move(plugin);
        setup_plugin();
        // receive events from plugin
        x_plugin->setListener(x_editor);
        // update Pd editor
        x_editor->setup();
        return true;
    } catch (const Error& e) {
        // shouldn't happen...
        pd_error(this, "%s: couldn't open '%s': %s",
                 classname(this), info->name.c_str(), e.what());
        return false;
    }
}

static void sendInfo(t_vstplugin *x, const char *what, const std::string& value){
    t_atom msg[2];
    SETSYMBOL(&msg[0], gensym(what));
    SETSYMBOL(&msg[1], gensym(value.c_str()));
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
    const PluginInfo *info = nullptr;
    if (argc > 0){ // some plugin
        auto path = atom_getsymbol(argv)->s_name;
        if (!(info = queryPlugin(x, path))){
            pd_error(x, "%s: couldn't open '%s' - no such file or plugin!", classname(x), path);
            return;
        }
    } else { // this plugin
        if (!x->check_plugin()) return;
        info = &x->x_plugin->info();
    }
    if (info){
        sendInfo(x, "path", info->path);
        sendInfo(x, "name", info->name);
        sendInfo(x, "vendor", info->vendor);
        sendInfo(x, "category", info->category);
        sendInfo(x, "version", info->version);
        sendInfo(x, "inputs", info->numInputs);
        sendInfo(x, "outputs", info->numOutputs);
        sendInfo(x, "auxinputs", info->numAuxInputs);
        sendInfo(x, "auxoutputs", info->numAuxOutputs);
        sendInfo(x, "id", ("0x"+info->uniqueID));
        sendInfo(x, "editor", info->hasEditor());
        sendInfo(x, "synth", info->isSynth());
        sendInfo(x, "single", info->singlePrecision());
        sendInfo(x, "double", info->doublePrecision());
        sendInfo(x, "midiin", info->midiInput());
        sendInfo(x, "midiout", info->midiOutput());
        sendInfo(x, "sysexin", info->sysexInput());
        sendInfo(x, "sysexout", info->sysexOutput());
    }
}

// query plugin for capabilities
static void vstplugin_can_do(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    int result = x->x_plugin->canDo(s->s_name);
    t_atom msg[2];
    SETSYMBOL(&msg[0], s);
    SETFLOAT(&msg[1], result);
    outlet_anything(x->x_messout, gensym("can_do"), 2, msg);
}

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
    char *data = nullptr;
    int size = argc - 3;
    if (size > 0){
        data = (char *)getbytes(size);
        for (int i = 0, j = 3; i < size; ++i, ++j){
            data[i] = atom_getfloat(argv + j);
        }
    }
    intptr_t result = x->x_plugin->vendorSpecific(index, value, data, opt);
    t_atom msg[2];
    SETFLOAT(&msg[0], result);
    SETSYMBOL(&msg[1], gensym(toHex(result).c_str()));
    outlet_anything(x->x_messout, gensym("vendor_method"), 2, msg);
    if (data){
        freebytes(data, size);
    }
}

// print plugin info in Pd console
static void vstplugin_print(t_vstplugin *x){
    if (!x->check_plugin()) return;
    auto& info = x->x_plugin->info();
    post("~~~ VST plugin info ~~~");
    post("name: %s", info.name.c_str());
    post("path: %s", info.path.c_str());
    post("vendor: %s", info.vendor.c_str());
    post("category: %s", info.category.c_str());
    post("version: %s", info.version.c_str());
    post("input channels: %d", info.numInputs);
    post("output channels: %d", info.numOutputs);
    if (info.numAuxInputs > 0){
        post("aux input channels: %d", info.numAuxInputs);
    }
    if (info.numAuxOutputs > 0){
        post("aux output channels: %d", info.numAuxOutputs);
    }
    post("single precision: %s", info.singlePrecision() ? "yes" : "no");
    post("double precision: %s", info.doublePrecision() ? "yes" : "no");
    post("editor: %s", info.hasEditor() ? "yes" : "no");
    post("number of parameters: %d", info.numParameters());
    post("number of programs: %d", info.numPrograms());
    post("synth: %s", info.isSynth() ? "yes" : "no");
    post("midi input: %s", info.midiInput() ? "yes" : "no");
    post("midi output: %s", info.midiOutput() ? "yes" : "no");
    post("");
}

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

// reset the plugin
static void vstplugin_reset(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_plugin->suspend();
    x->x_plugin->resume();
}

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

static void vstplugin_click(t_vstplugin *x){
    vstplugin_vis(x, 1);
}

/*------------------------ transport----------------------------------*/

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

/*------------------------------------ parameters ------------------------------------------*/

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
    if (index >= 0 && index < x->x_plugin->info().numParameters()){
        t_atom msg[3];
        SETFLOAT(&msg[0], index);
        SETFLOAT(&msg[1], x->x_plugin->getParameter(index));
        SETSYMBOL(&msg[2], gensym(x->x_plugin->getParameterString(index).c_str()));
        outlet_anything(x->x_messout, gensym("param_state"), 3, msg);
    } else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
    }
}

// get parameter info (name + label + ...)
static void vstplugin_param_info(t_vstplugin *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    auto& info = x->x_plugin->info();
    if (index >= 0 && index < info.numParameters()){
        t_atom msg[3];
        SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(info.parameters[index].name.c_str()));
        SETSYMBOL(&msg[2], gensym(info.parameters[index].label.c_str()));
        // LATER add more info
        outlet_anything(x->x_messout, gensym("param_info"), 3, msg);
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
static void vstplugin_param_list(t_vstplugin *x){
    if (!x->check_plugin()) return;
    int n = x->x_plugin->info().numParameters();
    for (int i = 0; i < n; ++i){
        vstplugin_param_info(x, i);
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

/*------------------------------------- MIDI -----------------------------------------*/

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

/* --------------------------------- programs --------------------------------- */

// set the current program by index
static void vstplugin_program_set(t_vstplugin *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->info().numPrograms()){
        x->x_plugin->setProgram(index);
        x->x_editor->update();
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
static void vstplugin_program_list(t_vstplugin *x){
    if (!x->check_plugin()) return;
    int n = x->x_plugin->info().numPrograms();
    t_atom msg[2];
    for (int i = 0; i < n; ++i){
        SETFLOAT(&msg[0], i);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getProgramNameIndexed(i).c_str()));
        outlet_anything(x->x_messout, gensym("program_name"), 2, msg);
    }
}

/* -------------------------------- presets ---------------------------------------*/

// set program/bank data (list of bytes)
template<bool bank>
static void vstplugin_preset_data_set(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    std::string buffer;
    buffer.resize(argc);
    for (int i = 0; i < argc; ++i){
           // first clamp to 0-255, then assign to char (not 100% portable...)
        buffer[i] = (unsigned char)atom_getfloat(argv + i);
    }
    try {
        if (bank)
            x->x_plugin->readBankData(buffer);
        else
            x->x_plugin->readProgramData(buffer);
        x->x_editor->update();
    } catch (const Error& e) {
        pd_error(x, "%s: couldn't set %s data: %s",
                 classname(x), (bank ? "bank" : "program"), e.what());
    }
}

// get program/bank data
template<bool bank>
static void vstplugin_preset_data_get(t_vstplugin *x){
    if (!x->check_plugin()) return;
    std::string buffer;
    try {
        if (bank)
            x->x_plugin->writeBankData(buffer);
        else
            x->x_plugin->writeProgramData(buffer);
    } catch (const Error& e){
        pd_error(x, "%s: couldn't get %s data: %s",
                 classname(x), (bank ? "bank" : "program"), e.what());
        return;
    }
    const int n = buffer.size();
    std::vector<t_atom> atoms;
    atoms.resize(n);
    for (int i = 0; i < n; ++i){
            // first convert to range 0-255, then assign to t_float (not 100% portable...)
        SETFLOAT(&atoms[i], (unsigned char)buffer[i]);
    }
    outlet_anything(x->x_messout, gensym(bank ? "bank_data" : "program_data"),
                    n, atoms.data());
}

// read program/bank file (.FXP/.FXB)
template<bool bank>
static void vstplugin_preset_read(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char dir[MAXPDSTRING], *name;
    int fd = canvas_open(x->x_canvas, s->s_name, "", dir, &name, MAXPDSTRING, 1);
    if (fd < 0){
        pd_error(x, "%s: couldn't read %s file '%s' - no such file!",
                 classname(x), (bank ? "bank" : "program"), s->s_name);
        return;
    }
    sys_close(fd);
    char path[MAXPDSTRING];
    snprintf(path, MAXPDSTRING, "%s/%s", dir, name);
    // sys_bashfilename(path, path);
    try {
        if (bank)
            x->x_plugin->readBankFile(path);
        else
            x->x_plugin->readProgramFile(path);
        x->x_editor->update();
    } catch (const Error& e) {
        pd_error(x, "%s: couldn't read %s file '%s':\n%s",
                 classname(x), s->s_name, (bank ? "bank" : "program"), e.what());
    }
}

// write program/bank file (.FXP/.FXB)
template<bool bank>
static void vstplugin_preset_write(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char path[MAXPDSTRING];
    canvas_makefilename(x->x_canvas, s->s_name, path, MAXPDSTRING);
    try {
        if (bank)
            x->x_plugin->writeBankFile(path);
        else
            x->x_plugin->writeProgramFile(path);

    } catch (const Error& e){
        pd_error(x, "%s: couldn't write %s file '%s':\n%s",
                 classname(x), (bank ? "bank" : "program"), s->s_name, e.what());
    }
}

static void vstplugin_preset_count(t_vstplugin *x){
    if (!x->check_plugin()) return;
    t_atom msg;
    SETFLOAT(&msg, x->x_plugin->info().numPresets());
    outlet_anything(x->x_messout, gensym("preset_count"), 1, &msg);
}

static void vstplugin_preset_info(t_vstplugin *x, t_floatarg f){
    if (!x->check_plugin()) return;
    int index = f;
    if (index >= 0 && index < x->x_plugin->info().numPresets()){
        auto& preset = x->x_plugin->info().presets[index];
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
            bug("vstplugin_preset_list");
            break;
        }
        t_atom msg[4];
        SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(preset.name.c_str()));
        SETSYMBOL(&msg[2], gensym(preset.path.c_str()));
        SETFLOAT(&msg[3], type);
        outlet_anything(x->x_messout, gensym("preset_info"), 4, msg);
    } else {
        pd_error(x, "%s: preset index %d out of range!", classname(x), index);
    }
}

static void vstplugin_preset_list(t_vstplugin *x){
    if (!x->check_plugin()) return;
    int n = x->x_plugin->info().numPresets();
    t_atom msg[4];
    for (int i = 0; i < n; ++i){
        vstplugin_preset_info(x, i);
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
    } else if (x->x_preset){
        int index = x->x_plugin->info().findPreset(x->x_preset->s_name);
        if (index >= 0){
            return index;
        } else {
            pd_error(x, "%s: couldn't find (current) preset '%s'!",
                     classname(x), x->x_preset->s_name);
        }
    } else {
        pd_error(x, "%s: no current preset!", classname(x));
    }
    return -1;
}

static bool vstplugin_preset_writeable(t_vstplugin *x, int index){
    bool writeable = x->x_plugin->info().presets[index].type == PresetType::User;
    if (!writeable){
        pd_error(x, "%s: preset is not writeable!", classname(x));
    }
    return writeable;
}

static void vstplugin_preset_load(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    auto& info = x->x_plugin->info();
    int index = vstplugin_preset_index(x, argc, argv);
    if (index < 0) return;

    vstplugin_preset_read<false>(x, gensym(info.presets[index].path.c_str()));
    x->x_preset = gensym(info.presets[index].name.c_str());
}


static void vstplugin_preset_notify(t_vstplugin *x){
    auto thing = gensym(t_vstplugin::glob_recv_name)->s_thing;
    if (thing){
        // notify all vstplugin~ instances for preset changes
        pd_vmess(thing, gensym("preset_change"), (char *)"s", x->x_key);
    }
}

static void vstplugin_preset_change(t_vstplugin *x, t_symbol *s){
    // only forward message to matching instances
    if (s == x->x_key){
        outlet_anything(x->x_messout, gensym("preset_change"), 0, 0);
    }
}

static void vstplugin_preset_save(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;

    bool newPreset = false;
    auto& info = x->x_plugin->info();
    int index = vstplugin_preset_index(x, argc, argv, false);
    if (index < 0){
        // if the preset couldn't be found and it's a (valid) symbol, make a new preset
        t_symbol *sym = atom_getsymbolarg(0, argc, argv);
        if (*sym->s_name){
            index = const_cast<PluginInfo&>(info).addPreset(sym->s_name);
            newPreset = true;
        }
    }
    if (index >= 0 && vstplugin_preset_writeable(x, index)){
        vstplugin_preset_write<false>(x, gensym(info.presets[index].path.c_str()));
        x->x_preset = gensym(info.presets[index].name.c_str());
        if (newPreset){
            vstplugin_preset_notify(x);
        }
    }
}

static void vstplugin_preset_rename(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    // [preset] newname
    if (!argc){
        return;
    }
    t_symbol *newname = atom_getsymbolarg((argc > 1), argc, argv);
    if (!(*newname->s_name)){
        pd_error(x, "%s: bad preset name %s!", classname(x), newname->s_name);
        return;
    }
    int index = vstplugin_preset_index(x, (argc > 1), argv);
    if (index < 0) return;

    // check if we rename the current preset
    bool update = x->x_preset && x->x_preset->s_name == x->x_plugin->info().presets[index].name;

    if (vstplugin_preset_writeable(x, index)){
        if (const_cast<PluginInfo&>(x->x_plugin->info()).renamePreset(index, newname->s_name)){
            if (update){
                x->x_preset = newname;
            }
            vstplugin_preset_notify(x);
        } else {
            pd_error(x, "%s: couldn't rename preset!", classname(x));
        }
    }
}

static void vstplugin_preset_delete(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    int index = vstplugin_preset_index(x, argc, argv);
    if (index < 0) return;

    // check if we delete the current preset
    if (x->x_preset && x->x_preset->s_name == x->x_plugin->info().presets[index].name){
        x->x_preset = nullptr;
    }

    if (vstplugin_preset_writeable(x, index)){
        if (const_cast<PluginInfo&>(x->x_plugin->info()).removePreset(index)){
            vstplugin_preset_notify(x);
        } else {
            pd_error(x, "%s: couldn't delete preset!", classname(x));
        }
    }
}

/*---------------------------- t_vstplugin (internal methods) -------------------------------------*/

static t_class *vstplugin_class;

// automated is true if parameter was set from the (generic) GUI, false if set by message ("param_set")
void t_vstplugin::set_param(int index, float value, bool automated){
    if (index >= 0 && index < x_plugin->info().numParameters()){
        value = std::max(0.f, std::min(1.f, value));
        int offset = x_plugin->getType() == PluginType::VST3 ? get_sample_offset() : 0;
        x_plugin->setParameter(index, value, offset);
        x_editor->param_changed(index, value, automated);
    } else {
        pd_error(this, "%s: parameter index %d out of range!", classname(this), index);
    }
}

void t_vstplugin::set_param(int index, const char *s, bool automated){
    if (index >= 0 && index < x_plugin->info().numParameters()){
        int offset = x_plugin->getType() == PluginType::VST3 ? get_sample_offset() : 0;
        if (!x_plugin->setParameter(index, s, offset)){
            pd_error(this, "%s: bad string value for parameter %d!", classname(this), index);
        }
            // some plugins don't just ignore bad string input but reset the parameter to some value...
        x_editor->param_changed(index, x_plugin->getParameter(index), automated);
    } else {
        pd_error(this, "%s: parameter index %d out of range!", classname(this), index);
    }
}

bool t_vstplugin::check_plugin(){
    if (x_plugin){
        return true;
    } else {
        pd_error(this, "%s: no plugin loaded!", classname(this));
        return false;
    }
}

void t_vstplugin::setup_plugin(){
    x_plugin->suspend();
    // check if precision is actually supported
    auto precision = x_precision;
    if (!x_plugin->info().hasPrecision(precision)){
        if (x_plugin->info().hasPrecision(ProcessPrecision::Single)){
            post("%s: '%s' doesn't support double precision, using single precision instead",
                 classname(this), x_plugin->info().name.c_str());
            precision = ProcessPrecision::Single;
        } else if (x_plugin->info().hasPrecision(ProcessPrecision::Double)){
            post("%s: '%s' doesn't support single precision, using double precision instead",
                 classname(this), x_plugin->info().name.c_str());
            precision = ProcessPrecision::Double;
        } else {
            post("%s: '%s' doesn't support single or double precision, bypassing",
                classname(this), x_plugin->info().name.c_str());
        }
    }
    x_plugin->setupProcessing(x_sr, x_blocksize, precision);
    x_plugin->setNumSpeakers(x_siginlets.size(), x_sigoutlets.size(),
                           x_sigauxinlets.size(), x_sigauxoutlets.size());
    x_plugin->resume();
    if (x_bypass != Bypass::Off){
        x_plugin->setBypass(x_bypass);
    }
}

int t_vstplugin::get_sample_offset(){
    int offset = clock_gettimesincewithunits(x_lastdsptime, 1, true);
    // LOG_DEBUG("sample offset: " << offset);
    return offset % x_blocksize;
}

// constructor
// usage: vstplugin~ [flags...] [file] inlets (default=2) outlets (default=2)
t_vstplugin::t_vstplugin(int argc, t_atom *argv){
    bool search = false; // search for plugins in the standard VST directories
    bool gui = true; // use GUI?
    bool keep = false; // remember plugin + state?
    // precision (defaults to Pd's precision)
    ProcessPrecision precision = (PD_FLOATSIZE == 64) ?
                ProcessPrecision::Double : ProcessPrecision::Single;
    t_symbol *file = nullptr; // plugin to open (optional)
    bool editor = false; // open plugin with VST editor?

    while (argc && argv->a_type == A_SYMBOL){
        const char *flag = argv->a_w.w_symbol->s_name;
        if (*flag == '-'){
            if (!strcmp(flag, "-n")){
                gui = false;
            } else if (!strcmp(flag, "-k")){
                keep = true;
            } else if (!strcmp(flag, "-e")){
                editor = true;
            } else if (!strcmp(flag, "-sp")){
                precision = ProcessPrecision::Single;
            } else if (!strcmp(flag, "-dp")){
                precision = ProcessPrecision::Double;
            } else if (!strcmp(flag, "-s")){
                search = true;
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
    // inputs (default: 2)
    int in = 2;
    if (argc > 0){
            // min. 1 because of CLASS_MAINSIGNALIN
        in = std::max<int>(1, atom_getfloat(argv));
    }
    // outputs (default: 2)
    int out = 2;
    if (argc > 1){
        out = std::max<int>(0, atom_getfloat(argv + 1));
    }
    // optional aux inputs/outputs
    int auxin = atom_getfloatarg(2, argc, argv);
    int auxout = atom_getfloatarg(3, argc, argv);

    x_keep = keep;
    x_precision = precision;
    x_canvas = canvas_getcurrent();
    x_editor = std::make_shared<t_vsteditor>(*this, gui);
    x_search_clock = clock_new(this, (t_method)vstplugin_search_done);

    // inlets (we already have a main inlet!)
    int totalin = in + auxin - 1;
    while (totalin--){
        inlet_new(&x_obj, &x_obj.ob_pd, &s_signal, &s_signal);
    }
    x_siginlets.resize(in);
    x_sigauxinlets.resize(auxin);
    // outlets:
    int totalout = out + auxout;
    while (totalout--){
        outlet_new(&x_obj, &s_signal);
    }
    x_sigoutlets.resize(out);
    x_sigauxoutlets.resize(auxout);
    x_messout = outlet_new(&x_obj, 0); // additional message outlet

    // search
    x_search_data = std::make_shared<t_search_data>();

    if (search && !gDidSearch){
        for (auto& path : getDefaultSearchPaths()){
            searchPlugins<false>(path, true); // synchronous and parallel
        }
    #if 1
        writeIniFile(); // shall we write cache file?
    #endif
        gDidSearch = true;
    }

    // open plugin
    if (file){
        open_plugin(file, editor);
        x_path = file; // HACK: set symbol for vstplugin_loadbang
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
        // placement new
    new (x) t_vstplugin(argc, argv);
    return x;
}

// destructor
t_vstplugin::~t_vstplugin(){
    vstplugin_close(this);
    x_search_data->s_running = false; // quit running async search!
    if (x_search_clock) clock_free(x_search_clock);
    LOG_DEBUG("vstplugin free");

    pd_unbind(&x_obj.ob_pd, gensym(glob_recv_name));
}

static void vstplugin_free(t_vstplugin *x){
    x->~t_vstplugin();
}

// perform routine

// TFloat: processing float type
// this templated method makes some optimization based on whether T and U are equal
template<typename TFloat>
static void vstplugin_doperform(t_vstplugin *x, int n){
    auto plugin = x->x_plugin.get();
    auto invec = (TFloat **)alloca(sizeof(TFloat *) * x->x_siginlets.size());
    auto outvec = (TFloat **)alloca(sizeof(TFloat *) * x->x_sigoutlets.size());
    auto auxinvec = (TFloat **)alloca(sizeof(TFloat *) * x->x_sigauxinlets.size());
    auto auxoutvec = (TFloat **)alloca(sizeof(TFloat *) * x->x_sigauxoutlets.size());

    auto prepareInput = [](auto vec, auto& inlets, auto buf, int k){
        for (size_t i = 0; i < inlets.size(); ++i, buf += k){
            // copy from Pd inlets into buffer
            if (i < inlets.size()){
                t_sample *in = inlets[i];
                if (std::is_same<t_sample, TFloat>::value){
                    std::copy(in, in + k, buf);
                } else {
                    for (int j = 0; j < k; ++j){
                        buf[j] = in[j]; // convert from t_sample to TFloat!
                    }
                }
            }
            vec[i] = buf; // point to buffer
        }
    };
    prepareInput(invec, x->x_siginlets, (TFloat *)x->x_inbuf.data(), n);
    prepareInput(auxinvec, x->x_sigauxinlets, (TFloat *)x->x_auxinbuf.data(), n);

        // prepare output buffer
    auto prepareOutput = [](auto vec, auto& outlets, auto buf, int k){
        for (size_t i = 0; i < outlets.size(); ++i, buf += k){
                // if t_sample and TFloat are the same, we can directly point to the outlet.
            if (std::is_same<t_sample, TFloat>::value){
                vec[i] = (TFloat *)outlets[i]; // have to trick the compiler (C++ 17 has constexpr if)
            } else {
                vec[i] = buf; // otherwise point to buffer
            }
        }
    };
    prepareOutput(outvec, x->x_sigoutlets, (TFloat *)x->x_outbuf.data(), n);
    prepareOutput(auxoutvec, x->x_sigauxoutlets, (TFloat *)x->x_auxoutbuf.data(), n);

        // process
    IPlugin::ProcessData<TFloat> data;
    data.input = (const TFloat **)invec;
    data.auxInput = (const TFloat **)auxinvec;
    data.output = outvec;
    data.auxOutput = auxoutvec;
    data.numSamples = n;
    data.numInputs = x->x_siginlets.size();
    data.numOutputs = x->x_sigoutlets.size();
    data.numAuxInputs = x->x_sigauxinlets.size();
    data.numAuxOutputs = x->x_sigauxoutlets.size();
    plugin->process(data);

    if (!std::is_same<t_sample, TFloat>::value){
            // copy output buffer to Pd outlets
        auto copyBuffer = [](auto vec, auto& outlets, int k){
            for (int i = 0; i < (int)outlets.size(); ++i){
                TFloat *src = vec[i];
                t_sample *dst = outlets[i];
                for (int j = 0; j < k; ++j){
                    dst[j] = src[j]; // converts from TFloat to t_sample!
                }
            }
        };
            // copy output buffer to Pd outlets
        copyBuffer(outvec, x->x_sigoutlets, n);
        copyBuffer(auxoutvec, x->x_sigauxoutlets, n);
    }
}

static t_int *vstplugin_perform(t_int *w){
    t_vstplugin *x = (t_vstplugin *)(w[1]);
    int n = (int)(w[2]);
    auto plugin = x->x_plugin.get();
    auto precision = x->x_precision;
    bool doit = plugin != nullptr;
    x->x_lastdsptime = clock_getlogicaltime();

    if (doit){
            // check processing precision (single or double)
        if (!plugin->info().hasPrecision(precision)){
            if (plugin->info().hasPrecision(ProcessPrecision::Single)){
                precision = ProcessPrecision::Single;
            } else if (plugin->info().hasPrecision(ProcessPrecision::Double)){
                precision = ProcessPrecision::Double;
            } else {
                doit = false; // maybe some VST2 MIDI plugins
            }
        }
    }
    if (doit){
        if (precision == ProcessPrecision::Double){
            vstplugin_doperform<double>(x, n);
        } else { // single precision
            vstplugin_doperform<float>(x, n);
        }
    } else {
        auto copyInput = [](auto& inlets, auto buf, auto numSamples){
            // copy inlets into temporary buffer (because inlets and outlets can alias!)
            for (size_t i = 0; i < inlets.size(); ++i, buf += numSamples){
                std::copy(inlets[i], inlets[i] + numSamples, buf);
            }
        };
        auto writeOutput = [](auto& outlets, auto buf, auto numInputs, auto numSamples){
            for (size_t i = 0; i < outlets.size(); ++i, buf += numSamples){
                if (i < numInputs){
                    std::copy(buf, buf + numSamples, outlets[i]); // copy
                } else {
                    std::fill(outlets[i], outlets[i] + numSamples, 0); // zero
                }
            }
        };
        // first copy both inputs and aux inputs!
        copyInput(x->x_siginlets, (t_sample *)x->x_inbuf.data(), n);
        copyInput(x->x_sigauxinlets, (t_sample *)x->x_auxinbuf.data(), n);
        // now write to output
        writeOutput(x->x_sigoutlets, (t_sample*)x->x_inbuf.data(), x->x_siginlets.size(), n);
        writeOutput(x->x_sigauxoutlets, (t_sample*)x->x_auxinbuf.data(), x->x_sigauxinlets.size(), n);
    }

    x->x_editor->flush_queues();

    return (w+3);
}

// loadbang
static void vstplugin_loadbang(t_vstplugin *x, t_floatarg action){
    // send message when plugin has been loaded (or failed to do so)
    // x_path is set in constructor
    if ((int)action == 0 && x->x_path){ // LB_LOAD
        bool success = x->x_plugin != nullptr;
        t_atom a;
        SETFLOAT(&a, success);
        outlet_anything(x->x_messout, gensym("open"), 1, &a);
        if (!success){
            x->x_path = nullptr; // undo HACK in vstplugin ctor
        }
    }
}

// save function
static void vstplugin_save(t_gobj *z, t_binbuf *bb){
    t_vstplugin *x = (t_vstplugin *)z;
    binbuf_addv(bb, "ssff", &s__X, gensym("obj"),
        (float)x->x_obj.te_xpix, (float)x->x_obj.te_ypix);
    binbuf_addbinbuf(bb, x->x_obj.ob_binbuf);
    binbuf_addsemi(bb);
    if (x->x_keep && x->x_plugin){
            // 1) plugin
        if (x->x_editor->vst_gui()){
            binbuf_addv(bb, "ssss", gensym("#A"), gensym("open"), gensym("-e"), x->x_path);
        } else {
            binbuf_addv(bb, "sss", gensym("#A"), gensym("open"), x->x_path);
        }
        binbuf_addsemi(bb);
            // 2) program number
        binbuf_addv(bb, "ssi", gensym("#A"), gensym("program_set"), x->x_plugin->getProgram());
        binbuf_addsemi(bb);
            // 3) program data
        std::string buffer;
        x->x_plugin->writeProgramData(buffer);
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

// dsp callback
static void vstplugin_dsp(t_vstplugin *x, t_signal **sp){
    int blocksize = sp[0]->s_n;
    t_float sr = sp[0]->s_sr;
    dsp_add(vstplugin_perform, 2, (t_int)x, (t_int)blocksize);
        // only reset plugin if blocksize or samplerate has changed
    if (x->x_plugin && (blocksize != x->x_blocksize || sr != x->x_sr)){
        x->setup_plugin();
    }
    x->x_blocksize = blocksize;
    x->x_sr = sr;
        // update signal vectors and buffers
    int k = 0;
        // main in:
    for (auto it = x->x_siginlets.begin(); it != x->x_siginlets.end(); ++it){
        *it = sp[k++]->s_vec;
    }
    x->x_inbuf.resize(x->x_siginlets.size() * sizeof(double) * blocksize); // large enough for double precision
        // aux in:
    for (auto it = x->x_sigauxinlets.begin(); it != x->x_sigauxinlets.end(); ++it){
        *it = sp[k++]->s_vec;
    }
    x->x_auxinbuf.resize(x->x_sigauxinlets.size() * sizeof(double) * blocksize);
        // main out:
    for (auto it = x->x_sigoutlets.begin(); it != x->x_sigoutlets.end(); ++it){
        *it = sp[k++]->s_vec;
    }
    x->x_outbuf.resize(x->x_sigoutlets.size() * sizeof(double) * blocksize);
        // aux out:
    for (auto it = x->x_sigauxoutlets.begin(); it != x->x_sigauxoutlets.end(); ++it){
        *it = sp[k++]->s_vec;
    }
    x->x_auxoutbuf.resize(x->x_sigauxoutlets.size() * sizeof(double) * blocksize);
}

// setup function
#ifdef _WIN32
#define EXPORT extern "C" __declspec(dllexport)
#elif __GNUC__ >= 4
#define EXPORT extern "C" __attribute__((visibility("default")))
#else
#define EXPORT extern "C"
#endif

EXPORT void vstplugin_tilde_setup(void){
    vstplugin_class = class_new(gensym("vstplugin~"), (t_newmethod)(void *)vstplugin_new,
        (t_method)vstplugin_free, sizeof(t_vstplugin), 0, A_GIMME, A_NULL);
    CLASS_MAINSIGNALIN(vstplugin_class, t_vstplugin, x_f);
    class_setsavefn(vstplugin_class, vstplugin_save);
    class_addmethod(vstplugin_class, (t_method)vstplugin_dsp, gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_loadbang, gensym("loadbang"), A_FLOAT, A_NULL);
    // plugin
    class_addmethod(vstplugin_class, (t_method)vstplugin_open, gensym("open"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_close, gensym("close"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_search, gensym("search"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_search_stop, gensym("search_stop"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_search_clear, gensym("search_clear"), A_DEFFLOAT, A_NULL);

    class_addmethod(vstplugin_class, (t_method)vstplugin_bypass, gensym("bypass"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_reset, gensym("reset"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_vis, gensym("vis"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_pos, gensym("pos"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_click, gensym("click"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_info, gensym("info"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_can_do, gensym("can_do"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_vendor_method, gensym("vendor_method"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_print, gensym("print"), A_NULL);
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
    // parameters
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_set, gensym("param_set"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_get, gensym("param_get"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_info, gensym("param_info"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_count, gensym("param_count"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_list, gensym("param_list"), A_NULL);
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
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_list, gensym("program_list"), A_NULL);
    // presets
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_count, gensym("preset_count"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_info, gensym("preset_info"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_list, gensym("preset_list"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_load, gensym("preset_load"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_save, gensym("preset_save"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_rename, gensym("preset_rename"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_delete, gensym("preset_delete"), A_GIMME, A_NULL);
    // read/write fx programs
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_data_set<false>, gensym("program_data_set"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_data_get<false>, gensym("program_data_get"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_read<false>, gensym("program_read"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_write<false>, gensym("program_write"), A_SYMBOL, A_NULL);
    // read/write fx banks
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_data_set<true>, gensym("bank_data_set"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_data_get<true>, gensym("bank_data_get"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_read<true>, gensym("bank_read"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_write<true>, gensym("bank_write"), A_SYMBOL, A_NULL);
    // private messages
    class_addmethod(vstplugin_class, (t_method)vstplugin_preset_change, gensym("preset_change"), A_SYMBOL, A_NULL);

    vstparam_setup();

    // read cached plugin info
    readIniFile();

#if !HAVE_UI_THREAD
    eventLoopClock = clock_new(0, (t_method)eventLoopTick);
    clock_delay(eventLoopClock, 0);
#endif

    post("vstplugin~ v%d.%d.%d%s", VERSION_MAJOR, VERSION_MINOR, VERSION_BUGFIX,
         VERSION_BETA ? " (beta)" : "");
#ifdef __APPLE__
    post("WARNING: on macOS, the VST GUI must run on the audio thread - use with care!");
#endif
}
