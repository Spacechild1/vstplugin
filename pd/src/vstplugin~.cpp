#include "vstplugin~.h"

#undef pd_class
#define pd_class(x) (*(t_pd *)(x))
#define classname(x) (class_getname(pd_class(x)))

#if !VSTTHREADS // don't use VST GUI threads
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
        error("couldn't read settings file:");
        error("%s", e.what());
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
        error("couldn't write settings file:");
        error("%s", e.what());
    }
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

static void addFactory(const std::string& path, IFactory::ptr factory){
    gPluginManager.addFactory(path, factory);
    for (int i = 0; i < factory->numPlugins(); ++i){
        auto plugin = factory->getPlugin(i);
        if (!plugin){
            bug("addFactory");
            return;
        }
        if (plugin->valid()){
            // also map bashed parameter names
            int num = plugin->parameters.size();
            for (int j = 0; j < num; ++j){
                auto key = plugin->parameters[j].name;
                bash_name(key);
                const_cast<PluginInfo&>(*plugin).paramMap[std::move(key)] = j;
            }
            // add plugin info
            auto key = makeKey(*plugin);
            gPluginManager.addPlugin(key, plugin);
            bash_name(key); // also add bashed version!
            gPluginManager.addPlugin(key, plugin);
        }
    }
}

// for asynchronous searching, we want to show the name of the plugin before
// the result, especially if the plugin takes a long time to load (e.g. shell plugins).
// The drawback is that we either have to post the result on a seperate line or post
// on the normal log level. For now, we do the latter.
class PdLog {
public:
    template <typename... T>
    PdLog(bool async, PdLogLevel level, const char *fmt, T... args)
        : PdLog(async, level)
    {
        if (async_){
            // post immediately!
            sys_lock();
            startpost(fmt, args...);
            sys_unlock();
            force_ = true; // force newline on destruction!
        } else {
            // defer posting
            char buf[MAXPDSTRING];
            snprintf(buf, MAXPDSTRING, fmt, args...);
            ss_ << buf;
        }
    }
    PdLog(bool async, PdLogLevel level)
        : level_(level), async_(async){}
    PdLog(PdLog&& other)
        : ss_(std::move(other.ss_)), level_(other.level_), async_(other.async_)
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
            if (async_){
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
        if (async_){
            sys_lock();
            verbose(PD_ERROR, "%s", e.what());
            sys_unlock();
        } else {
            verbose(PD_ERROR, "%s", e.what());
        }
        return *this;
    }
    PdLog& operator <<(ProbeResult result){
        switch (result){
        case ProbeResult::success:
            *this << "ok!";
            break;
        case ProbeResult::fail:
            *this << "failed!";
            break;
        case ProbeResult::crash:
            *this << "crashed!";
            break;
        default:
            bug("probePlugin");
            break;
        }
        return *this;
    }
private:
    std::stringstream ss_;
    PdLogLevel level_;
    bool async_;
    bool force_ = false;
};

template<typename T>
void consume(T&& obj){
    T dummy(std::move(obj));
}

template<typename... T>
void postBug(bool async, const char *fmt, T... args){
    if (async) sys_lock();
    bug(fmt, args...);
    if (async) sys_unlock();
}

template<typename... T>
void postError(bool async, const char *fmt, T... args){
    if (async) sys_lock();
    error(fmt, args...);
    if (async) sys_unlock();
}


// load factory and probe plugins
static IFactory::ptr probePlugin(const std::string& path, bool async = false){
    IFactory::ptr factory;

    if (gPluginManager.findFactory(path)){
        postBug(async, "probePlugin");
        return nullptr;
    }
    if (gPluginManager.isException(path)){
        PdLog log(async, PD_DEBUG,
                  "'%s' is black-listed", path.c_str());
        return nullptr;
    }

    try {
        factory = IFactory::load(path);
    } catch (const Error& e){
        PdLog log(async, PD_DEBUG,
                  "couldn't load '%s': %s", path.c_str(), e.what());
        gPluginManager.addException(path);
        return nullptr;
    }

    PdLog log(async, PD_DEBUG, "probing '%s'... ", path.c_str());

    try {
        factory->probe([&](const PluginInfo& desc, int which, int numPlugins){
            // Pd's posting methods have a size limit, so we log each plugin seperately!
            if (numPlugins > 1){
                if (which == 0){
                    consume(std::move(log));
                }
                PdLog log1(async, PD_DEBUG, "\t'");
                if (!desc.name.empty()){
                    log1 << desc.name << "' ... ";
                } else {
                    log1 << "plugin "; // e.g. "plugin crashed!"
                }
                log1 << desc.probeResult;
            } else {
                log << desc.probeResult;
                consume(std::move(log));
            }
        });
    } catch (const Error& e){
        log << "error";
        log << e;
        return nullptr;
    }

    if (factory->numPlugins() == 1){
        auto plugin = factory->getPlugin(0);
        if (plugin->valid()){
            // factories with a single plugin can also be aliased by their file path(s)
            gPluginManager.addPlugin(plugin->path, plugin);
            gPluginManager.addPlugin(path, plugin);
        }
    }

    if (factory->valid()){
        addFactory(path, factory);
        return factory;
    } else {
        gPluginManager.addException(path);
        return nullptr;
    }
}

static void searchPlugins(const std::string& path, t_vstplugin *x = nullptr, bool async = false){
    int count = 0;
    {
        std::string bashPath = path;
        sys_unbashfilename(&bashPath[0], &bashPath[0]);
        PdLog log(async, PD_NORMAL, "searching in '%s' ...", bashPath.c_str()); // destroy
    }

    vst::search(path, [&](const std::string& absPath, const std::string&){
        std::string pluginPath = absPath;
        sys_unbashfilename(&pluginPath[0], &pluginPath[0]);
        // check if module has already been loaded
        auto factory = gPluginManager.findFactory(pluginPath);
        if (factory){
            // just post paths of valid plugins
            PdLog log(async, PD_DEBUG, "%s", factory->path().c_str());
            auto numPlugins = factory->numPlugins();
            if (numPlugins == 1){
                auto plugin = factory->getPlugin(0);
                if (!plugin){
                    bug("searchPlugins");
                    return;
                }
                if (plugin->valid()){
                    auto key = makeKey(*plugin);
                    bash_name(key);
                    if (x){
                        x->x_plugins.push_back(gensym(key.c_str()));
                    }
                    count++;
                }
            } else {
                // Pd's posting methods have a size limit, so we log each plugin seperately!
                consume(std::move(log));
                for (int i = 0; i < numPlugins; ++i){
                    auto plugin = factory->getPlugin(i);
                    if (!plugin){
                        bug("searchPlugins");
                        return;
                    }
                    if (plugin->valid()){
                        auto key = makeKey(*plugin);
                        bash_name(key);
                        PdLog log1(async, PD_DEBUG, "\t");
                        log1 << plugin->name;
                        if (x){
                            x->x_plugins.push_back(gensym(key.c_str()));
                        }
                        count++;
                    }
                }
            }
        } else {
            // probe (will post results and add plugins)
            if ((factory = probePlugin(pluginPath, async))){
                for (int i = 0; i < factory->numPlugins(); ++i){
                    auto plugin = factory->getPlugin(i);
                    if (!plugin){
                        bug("searchPlugins");
                        return;
                    }
                    if (plugin->valid()){
                        if (x){
                            auto key = makeKey(*plugin);
                            bash_name(key);
                            x->x_plugins.push_back(gensym(key.c_str()));
                        }
                        count++;
                    }
                }
            }
        }
    });
    PdLog log(async, PD_NORMAL, "found %d plugin%s", count, (count == 1 ? "." : "s."));
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
    snprintf(buf, sizeof(buf), "%s", plugin.getParameterDisplay(index).c_str());
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
#if VSTTHREADS
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
#if VSTTHREADS
    bool vstgui = window();
        // we only need to lock for GUI windows, but never for the generic Pd editor
    if (vstgui){
        e_mutex.lock();
    }
#endif
    queue.push_back(std::forward<U>(event));
#if VSTTHREADS
    if (vstgui){
        e_mutex.unlock();
    }
        // sys_lock / sys_unlock are not recursive so we check if we are in the main thread
    auto id = std::this_thread::get_id();
    if (id != e_mainthread){
        // LOG_DEBUG("lock");
        sys_lock();
    }
#endif
    clock_delay(e_clock, 0);
#if VSTTHREADS
    if (id != e_mainthread){
        sys_unlock();
        // LOG_DEBUG("unlocked");
    }
#endif
}

    // parameter automation notification might come from another thread (VST plugin GUI).
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
#if VSTTHREADS
    bool vstgui = x->vst_gui();
        // we only need to lock if we have a GUI thread
    if (vstgui){
        // it's more important to not block than flushing the queues on time
        if (!x->e_mutex.try_lock()){
            LOG_DEBUG("couldn't lock mutex");
            return;
        }
    }
#endif
        // swap parameter, midi and sysex queues.
    std::vector<std::pair<int, float>> paramQueue;
    paramQueue.swap(x->e_automated);
    std::vector<MidiEvent> midiQueue;
    midiQueue.swap(x->e_midi);
    std::vector<SysexEvent> sysexQueue;
    sysexQueue.swap(x->e_sysex);

#if VSTTHREADS
    if (vstgui){
        x->e_mutex.unlock();
    }
#endif
        // NOTE: it is theoretically possible that outputting messages
        // will cause more messages to be sent (e.g. when being fed back into [vstplugin~]
        // and if there's no mutex involved this would modify a std::vector while being read.
        // one solution is to just double buffer via swap, so subsequent events will go to
        // a new empty queue. Although I *think* this might not be necessary for midi/sysex messages
        // I do it anyway. swapping a std::vector is cheap. also it minimizes the time spent
        // in the critical section.

        // automated parameters:
    for (auto& param : paramQueue){
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
        // midi events:
    for (auto& midi : midiQueue){
        t_atom msg[3];
        SETFLOAT(&msg[0], (unsigned char)midi.data[0]);
        SETFLOAT(&msg[1], (unsigned char)midi.data[1]);
        SETFLOAT(&msg[2], (unsigned char)midi.data[2]);
        outlet_anything(outlet, gensym("midi"), 3, msg);
    }
        // sysex events:
    for (auto& sysex : sysexQueue){
        std::vector<t_atom> msg;
        int n = sysex.data.size();
        msg.resize(n);
        for (int i = 0; i < n; ++i){
            SETFLOAT(&msg[i], (unsigned char)sysex.data[i]);
        }
        outlet_anything(outlet, gensym("midi"), n, msg.data());
    }
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

    send_vmess(gensym("rename"), (char *)"s", gensym(e_owner->x_plugin->getPluginName().c_str()));
    send_mess(gensym("clear"));

    int nparams = e_owner->x_plugin->getNumParameters();
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
        snprintf(buf, sizeof(buf), "%d: %s", i, e_owner->x_plugin->getParameterName(i).c_str());
        substitute_whitespace(buf);
        SETSYMBOL(slider+11, gensym(buf));
        send_mess(gensym("obj"), 21, slider);
            // create display
        SETFLOAT(display, xpos + 128 + 10); // slider + space
        SETFLOAT(display+1, ypos);
        SETSYMBOL(display+6, gensym(e_owner->x_plugin->getParameterLabel(i).c_str()));
        SETSYMBOL(display+7, e_params[i].p_display_rcv);
        SETSYMBOL(display+8, e_params[i].p_display_snd);
        send_mess(gensym("symbolatom"), 9, display);
    }
    float width = row_width * ncolumns + 2 * xoffset;
    float height = nrows * col_height + 2 * yoffset;
    if (width > 1000) width = 1000;
    send_vmess(gensym("setbounds"), "ffff", 0.f, 0.f, width, height);
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
        int n = e_owner->x_plugin->getNumParameters();
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

void t_vsteditor::vis(bool v){
    auto win = window();
    if (win){
        if (v){
            win->bringToTop();
        } else {
            win->hide();
        }
    } else if (e_canvas) {
        send_vmess(gensym("vis"), "i", (int)v);
    }
}

/*---------------- t_vstplugin (public methods) ------------------*/

// search
static void vstplugin_search_done(t_vstplugin *x){
        // for async search:
    if (x->x_thread.joinable()){
        x->x_thread.join();
    }
    verbose(PD_NORMAL, "search done");
    // sort plugin names alphabetically and case independent
    std::sort(x->x_plugins.begin(), x->x_plugins.end(), [](const auto& lhs, const auto& rhs){
        std::string s1 = lhs->s_name;
        std::string s2 = rhs->s_name;
        for (auto& c : s1) { c = std::tolower(c); }
        for (auto& c : s2) { c = std::tolower(c); }
        return s1 < s2;
    });
    for (auto& plugin : x->x_plugins){
        t_atom msg;
        SETSYMBOL(&msg, plugin);
        outlet_anything(x->x_messout, gensym("plugin"), 1, &msg);
    }
    outlet_anything(x->x_messout, gensym("search_done"), 0, nullptr);
}

static void vstplugin_search_threadfun(t_vstplugin *x, std::vector<std::string> searchPaths, bool update){
    for (auto& path : searchPaths){
        searchPlugins(path, x, true); // async
    }
    if (update){
        writeIniFile();
    }
    sys_lock();
    clock_delay(x->x_clock, 0); // schedules vstplugin_search_done
    sys_unlock();
}

static void vstplugin_search(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    bool async = false;
    bool update = true; // update cache file
    std::vector<std::string> searchPaths;

    if (x->x_thread.joinable()){
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

    x->x_plugins.clear(); // clear list of plug-in keys

    if (argc > 0){
        while (argc--){
            auto sym = atom_getsymbol(argv++);
            char path[MAXPDSTRING];
            canvas_makefilename(x->x_canvas, sym->s_name, path, MAXPDSTRING);
            if (async){
                searchPaths.emplace_back(path); // save for later
            } else {
                searchPlugins(path, x);
            }
        }
    } else {  // search in the default VST search paths if no user paths were provided
        for (auto& path : getDefaultSearchPaths()){
            if (async){
                searchPaths.emplace_back(path); // save for later
            } else {
                searchPlugins(path, x);
            }
        }
    }

    if (async){
            // spawn thread which does the actual searching in the background
        x->x_thread = std::thread(vstplugin_search_threadfun, x, std::move(searchPaths), update);
    } else {
        if (update){
            writeIniFile();
        }
        vstplugin_search_done(x);
    }
}

static void vstplugin_search_clear(t_vstplugin *x, t_floatarg f){
        // clear the plugin description dictionary
    gPluginManager.clear();
    if (f != 0){
        removeFile(getSettingsDir() + "/" SETTINGS_FILE);
    }
}

// resolves relative paths to an existing plugin in the canvas search paths or VST search paths.
// returns empty string on failure!
static std::string resolvePath(t_canvas *c, const std::string& s){
    std::string result;
        // resolve relative path
    if (!sys_isabsolutepath(s.c_str())){
        std::string path = s;
        char buf[MAXPDSTRING+1];
    #ifdef _WIN32
        const char *ext = ".dll";
    #elif defined(__APPLE__)
        const char *ext = ".vst";
    #else // Linux/BSD/etc.
        const char *ext = ".so";
    #endif
        if (path.find(".vst3") == std::string::npos && path.find(ext) == std::string::npos){
            path += ext;
        }
            // first try canvas search paths
        char dirresult[MAXPDSTRING];
        char *name = nullptr;
    #ifdef __APPLE__
        const char *bundleinfo = "/Contents/Info.plist";
        // on MacOS VST plugins are bundles (directories) but canvas_open needs a real file
        int fd = canvas_open(c, (path + bundleinfo).c_str(), "", dirresult, &name, MAXPDSTRING, 1);
    #else
        int fd = canvas_open(c, path.c_str(), "", dirresult, &name, MAXPDSTRING, 1);
    #endif
        if (fd >= 0){
            sys_close(fd);
            snprintf(buf, MAXPDSTRING, "%s/%s", dirresult, name);
    #ifdef __APPLE__
            char *find = strstr(buf, bundleinfo);
            if (find){
                *find = 0; // restore the bundle path
            }
    #endif
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
            if (probePlugin(abspath)){
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
        x->x_path = nullptr;
    }
}

// open
static void vstplugin_open(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    const PluginInfo *info = nullptr;
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
                // don't reopen the same plugin (mainly for -k flag)
            if (pathsym == x->x_path && x->x_editor->vst_gui() == editor){
                return;
            }
            break;
        }
    }
    if (!pathsym){
        pd_error(x, "%s: 'open' needs a symbol argument!", classname(x));
        return;
    }
    if (!(info = queryPlugin(x, pathsym->s_name))){
        pd_error(x, "%s: can't load '%s'", classname(x), pathsym->s_name);
        return;
    }
    if (!info->valid()){
        pd_error(x, "%s: can't use plugin '%s'", classname(x), info->path.c_str());
        return;
    }
        // *now* close the old plugin
    vstplugin_close(x);
        // open the new VST plugin
    try {
        IPlugin::ptr plugin;
        if (editor){
            plugin = UIThread::create(*info);
        } else {
            plugin = info->create();
        }
        x->x_uithread = editor;
        x->x_path = pathsym; // store path symbol (to avoid reopening the same plugin)
        post("opened VST plugin '%s'", plugin->getPluginName().c_str());
        plugin->suspend();
            // initially, blocksize is 0 (before the 'dsp' message is sent).
            // some plugins might not like 0, so we send some sane default size.
        plugin->setBlockSize(x->x_blocksize > 0 ? x->x_blocksize : 64);
        plugin->setSampleRate(x->x_sr);
        int nin = std::min<int>(plugin->getNumInputs(), x->x_siginlets.size());
        int nout = std::min<int>(plugin->getNumOutputs(), x->x_sigoutlets.size());
        plugin->setNumSpeakers(nin, nout);
        plugin->resume();
        x->x_plugin = std::move(plugin);
            // receive events from plugin
        x->x_plugin->setListener(x->x_editor);
        x->update_precision();
        x->update_buffer();
        x->x_editor->setup();
    } catch (const Error& e) {
            // shouldn't happen...
        pd_error(x, "%s: couldn't open '%s': %s",
                 classname(x), info->name.c_str(), e.what());
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
        sendInfo(x, "id", toHex(info->id));
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
    post("single precision: %s", x->x_plugin->hasPrecision(ProcessPrecision::Single) ? "yes" : "no");
    post("double precision: %s", x->x_plugin->hasPrecision(ProcessPrecision::Double) ? "yes" : "no");
    post("editor: %s", x->x_plugin->hasEditor() ? "yes" : "no");
    post("number of parameters: %d", x->x_plugin->getNumParameters());
    post("number of programs: %d", x->x_plugin->getNumPrograms());
    post("synth: %s", x->x_plugin->isSynth() ? "yes" : "no");
    post("midi input: %s", x->x_plugin->hasMidiInput() ? "yes" : "no");
    post("midi output: %s", x->x_plugin->hasMidiOutput() ? "yes" : "no");
    post("");
}

// bypass the plugin
static void vstplugin_bypass(t_vstplugin *x, t_floatarg f){
    x->x_bypass = (f != 0);
    if (x->x_plugin){
        if (x->x_bypass){
            x->x_plugin->suspend();
        } else {
            x->x_plugin->resume();
        }
    }
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

static void vstplugin_click(t_vstplugin *x){
    vstplugin_vis(x, 1);
}

// set processing precision (single or double)
static void vstplugin_precision(t_vstplugin *x, t_symbol *s){
    if (s == gensym("single")){
        x->x_dp = 0;
    } else if (s == gensym("double")){
        x->x_dp = 1;
    } else {
        pd_error(x, "%s: bad argument to 'precision' message - must be 'single' or 'double'", classname(x));
        return;
    }
    x->update_precision();
        // clear the input buffer to avoid garbage in subsequent channels when the precision changes.
    memset(x->x_inbuf.data(), 0, x->x_inbuf.size()); // buffer is char array
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
        auto& map = x->x_plugin->info().paramMap;
        auto name = a->a_w.w_symbol->s_name;
        auto it = map.find(name);
        if (it == map.end()){
            pd_error(x, "%s: couldn't find parameter '%s'", classname(x), name);
            return false;
        }
        index = it->second;
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
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
        t_atom msg[3];
		SETFLOAT(&msg[0], index);
        SETFLOAT(&msg[1], x->x_plugin->getParameter(index));
        SETSYMBOL(&msg[2], gensym(x->x_plugin->getParameterDisplay(index).c_str()));
        outlet_anything(x->x_messout, gensym("param_state"), 3, msg);
	} else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
	}
}

// get parameter info (name + label + ...)
static void vstplugin_param_info(t_vstplugin *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
        t_atom msg[3];
		SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getParameterName(index).c_str()));
        SETSYMBOL(&msg[2], gensym(x->x_plugin->getParameterLabel(index).c_str()));
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
    SETFLOAT(&msg, x->x_plugin->getNumParameters());
	outlet_anything(x->x_messout, gensym("param_count"), 1, &msg);
}

// list parameters (index + info)
static void vstplugin_param_list(t_vstplugin *x){
    if (!x->check_plugin()) return;
    int n = x->x_plugin->getNumParameters();
	for (int i = 0; i < n; ++i){
        vstplugin_param_info(x, i);
	}
}

// list parameter states (index + value)
static void vstplugin_param_dump(t_vstplugin *x){
    if (!x->check_plugin()) return;
    int n = x->x_plugin->getNumParameters();
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
    for (int i = 0; i < 3 && i < argc; ++i){
        event.data[i] = atom_getfloat(argv + i);
    }
    x->x_plugin->sendMidiEvent(event);
}

// helper function
static void vstplugin_midi_mess(t_vstplugin *x, int onset, int channel, int v1, int v2 = 0){
    t_atom atoms[3];
    channel = std::max(1, std::min(16, (int)channel)) - 1;
    v1 = std::max(0, std::min(127, v1));
    v2 = std::max(0, std::min(127, v2));
    SETFLOAT(&atoms[0], channel + onset);
    SETFLOAT(&atoms[1], v1);
    SETFLOAT(&atoms[2], v2);
    vstplugin_midi_raw(x, 0, 3, atoms);
}

// send MIDI messages (convenience methods)
static void vstplugin_midi_noteoff(t_vstplugin *x, t_floatarg channel, t_floatarg pitch, t_floatarg velocity){
    vstplugin_midi_mess(x, 128, channel, pitch, velocity);
}

static void vstplugin_midi_note(t_vstplugin *x, t_floatarg channel, t_floatarg pitch, t_floatarg velocity){
    vstplugin_midi_mess(x, 144, channel, pitch, velocity);
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

    x->x_plugin->sendSysexEvent(SysexEvent(std::move(data)));
}

/* --------------------------------- programs --------------------------------- */

// set the current program by index
static void vstplugin_program_set(t_vstplugin *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumPrograms()){
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
    SETFLOAT(&msg, x->x_plugin->getNumPrograms());
	outlet_anything(x->x_messout, gensym("program_count"), 1, &msg);
}

// list all programs (index + name)
static void vstplugin_program_list(t_vstplugin *x){
    int n = x->x_plugin->getNumPrograms();
    t_atom msg[2];
    for (int i = 0; i < n; ++i){
        SETFLOAT(&msg[0], i);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getProgramNameIndexed(i).c_str()));
        outlet_anything(x->x_messout, gensym("program_name"), 2, msg);
    }
}

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
            x->x_plugin->readProgramFile(path);

    } catch (const Error& e){
        pd_error(x, "%s: couldn't write %s file '%s':\n%s",
                 classname(x), (bank ? "bank" : "program"), s->s_name, e.what());
    }
}

/*---------------------------- t_vstplugin (internal methods) -------------------------------------*/

static t_class *vstplugin_class;

// automated is true if parameter was set from the (generic) GUI, false if set by message ("param_set")
void t_vstplugin::set_param(int index, float value, bool automated){
    if (index >= 0 && index < x_plugin->getNumParameters()){
        value = std::max(0.f, std::min(1.f, value));
        x_plugin->setParameter(index, value);
        x_editor->param_changed(index, value, automated);
    } else {
        pd_error(this, "%s: parameter index %d out of range!", classname(this), index);
    }
}

void t_vstplugin::set_param(int index, const char *s, bool automated){
    if (index >= 0 && index < x_plugin->getNumParameters()){
        if (!x_plugin->setParameter(index, s)){
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

void t_vstplugin::update_buffer(){
        // this routine is called in the "dsp" method and when a plugin is loaded.
    int nin = x_siginlets.size();
    int nout = x_sigoutlets.size();
    int pin = 0;
    int pout = 0;
    if (x_plugin){
        pin = x_plugin->getNumInputs();
        pout = x_plugin->getNumOutputs();
    }
        // the input/output buffers must be large enough to fit both
        // the number of Pd inlets/outlets and plugin inputs/outputs
    int ninvec = std::max(pin, nin);
    int noutvec = std::max(pout, nout);
        // first clear() so that resize() will zero all values
    x_inbuf.clear();
    x_outbuf.clear();
        // make it large enough for double precision
    x_inbuf.resize(ninvec * sizeof(double) * x_blocksize);
    x_outbuf.resize(noutvec * sizeof(double) * x_blocksize);
    x_invec.resize(ninvec);
    x_outvec.resize(noutvec);
    LOG_DEBUG("vstplugin~: updated buffer");
}

void t_vstplugin::update_precision(){
        // set desired precision
    int dp = x_dp;
        // check precision
    if (x_plugin){
        if (!x_plugin->hasPrecision(ProcessPrecision::Single) && !x_plugin->hasPrecision(ProcessPrecision::Double)) {
            post("%s: '%s' doesn't support single or double precision, bypassing",
                classname(this), x_plugin->getPluginName().c_str());
            return;
        }
        if (x_dp && !x_plugin->hasPrecision(ProcessPrecision::Double)){
            post("%s: '%s' doesn't support double precision, using single precision instead",
                 classname(this), x_plugin->getPluginName().c_str());
            dp = 0;
        }
        else if (!x_dp && !x_plugin->hasPrecision(ProcessPrecision::Single)){ // very unlikely...
            post("%s: '%s' doesn't support single precision, using double precision instead",
                 classname(this), x_plugin->getPluginName().c_str());
            dp = 1;
        }
            // set the actual precision
        x_plugin->setPrecision(dp ? ProcessPrecision::Double : ProcessPrecision::Single);
    }
}

// constructor
// usage: vstplugin~ [flags...] [file] inlets (default=2) outlets (default=2)
t_vstplugin::t_vstplugin(int argc, t_atom *argv){
    bool search = false; // search for plugins in the standard VST directories
    bool gui = true; // use GUI?
    bool keep = false; // remember plugin + state?
    bool dp = (PD_FLOATSIZE == 64); // use double precision? default to precision of Pd
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
                dp = false;
            } else if (!strcmp(flag, "-dp")){
                dp = true;
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
        // signal inlets (default: 2)
    int in = 2;
    if (argc > 0){
            // min. 1 because of CLASS_MAINSIGNALIN
        in = std::max<int>(1, atom_getfloat(argv));
    }
        // signal outlets (default: 2)
    int out = 2;
    if (argc > 1){
        out = std::max<int>(0, atom_getfloat(argv + 1));
    }

    x_keep = keep;
    x_dp = dp;
    x_canvas = canvas_getcurrent();
    x_editor = std::make_shared<t_vsteditor>(*this, gui);
    x_clock = clock_new(this, (t_method)vstplugin_search_done);

        // inlets (skip first):
    for (int i = 1; i < in; ++i){
        inlet_new(&x_obj, &x_obj.ob_pd, &s_signal, &s_signal);
	}
    x_siginlets.resize(in);
        // outlets:
	for (int i = 0; i < out; ++i){
        outlet_new(&x_obj, &s_signal);
    }
    x_messout = outlet_new(&x_obj, 0); // additional message outlet
    x_sigoutlets.resize(out);

    if (search && !gDidSearch){
        for (auto& path : getDefaultSearchPaths()){
            searchPlugins(path);
        }
        // shall we write cache file?
    #if 1
        writeIniFile();
    #endif
        gDidSearch = true;
    }

    if (file){
        t_atom msg[2];
        if (editor){
            SETSYMBOL(&msg[0], gensym("-e"));
            SETSYMBOL(&msg[1], file);
        } else {
            SETSYMBOL(&msg[0], file);
        }
        vstplugin_open(this, 0, (int)editor + 1, msg);
    }
    t_symbol *asym = gensym("#A");
        // bashily unbind #A
    asym->s_thing = 0;
        // now bind #A to us to receive following messages
    pd_bind(&x_obj.ob_pd, asym);
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
    if (x_thread.joinable()){
        x_thread.join();
    }
    if (x_clock) clock_free(x_clock);
    LOG_DEBUG("vstplugin free");
}

static void vstplugin_free(t_vstplugin *x){
    x->~t_vstplugin();
}

// perform routine

// TFloat: processing float type
// this templated method makes some optimization based on whether T and U are equal
template<typename TFloat>
static void vstplugin_doperform(t_vstplugin *x, int n, bool bypass){
    int nin = x->x_siginlets.size();
    t_sample ** sigin = x->x_siginlets.data();
    int nout = x->x_sigoutlets.size();
    t_sample ** sigout = x->x_sigoutlets.data();
    char *inbuf = x->x_inbuf.data();
    int ninvec = x->x_invec.size();
    void ** invec = x->x_invec.data();
    char *outbuf = x->x_outbuf.data();
    int noutvec = x->x_outvec.size();
    void ** outvec = x->x_outvec.data();
    int out_offset = 0;
    auto plugin = x->x_plugin.get();

    if (!bypass){  // process audio
        int pout = plugin->getNumOutputs();
        out_offset = pout;
            // prepare input buffer + pointers
        for (int i = 0; i < ninvec; ++i){
            TFloat *buf = (TFloat *)inbuf + i * n;
            invec[i] = buf;
            if (i < nin){  // copy from Pd inlets
                t_sample *in = sigin[i];
                for (int j = 0; j < n; ++j){
                    buf[j] = in[j];
                }
            } else if (std::is_same<t_sample, double>::value
                       && std::is_same<TFloat, float>::value){
                    // we only have to zero for this special case: 'bypass' could
                    // have written doubles into the input buffer, leaving garbage in
                    // subsequent channels when the buffer is reinterpreted as floats.
                for (int j = 0; j < n; ++j){
                    buf[j] = 0;
                }
            }
        }
            // set output buffer pointers
        for (int i = 0; i < pout; ++i){
                // if t_sample and TFloat are the same, we can directly write to the outlets.
            if (std::is_same<t_sample, TFloat>::value && i < nout){
                outvec[i] = sigout[i];
            } else {
                outvec[i] = (TFloat *)outbuf + i * n;
            }
        }
            // process
        if (std::is_same<TFloat, float>::value){
            plugin->process((const float **)invec, (float **)outvec, n);
        } else {
            plugin->processDouble((const double **)invec, (double **)outvec, n);
        }

        if (!std::is_same<t_sample, TFloat>::value){
                // copy output buffer to Pd outlets
            for (int i = 0; i < nout && i < pout; ++i){
                t_sample *out = sigout[i];
                double *buf = (double *)outvec[i];
                for (int j = 0; j < n; ++j){
                    out[j] = buf[j];
                }
            }
        }
    } else {  // just pass it through
        t_sample *buf = (t_sample *)inbuf;
        out_offset = nin;
            // copy input
        for (int i = 0; i < nin && i < nout; ++i){
            t_sample *in = sigin[i];
            t_sample *bufptr = buf + i * n;
            for (int j = 0; j < n; ++j){
                bufptr[j] = in[j];
            }
        }
            // write output
        for (int i = 0; i < nin && i < nout; ++i){
            t_sample *out = sigout[i];
            t_sample *bufptr = buf + i * n;
            for (int j = 0; j < n; ++j){
                out[j] = bufptr[j];
            }
        }
    }
        // zero remaining outlets
    for (int i = out_offset; i < nout; ++i){
        t_sample *out = sigout[i];
        for (int j = 0; j < n; ++j){
            out[j] = 0;
        }
    }
}

static t_int *vstplugin_perform(t_int *w){
    t_vstplugin *x = (t_vstplugin *)(w[1]);
    int n = (int)(w[2]);
    auto plugin = x->x_plugin.get();
    bool dp = x->x_dp;
    bool bypass = plugin ? x->x_bypass : true;

    if (plugin && !bypass) {
            // check processing precision (single or double)
        if (!plugin->hasPrecision(ProcessPrecision::Single)
                && !plugin->hasPrecision(ProcessPrecision::Double)) { // very unlikely...
            bypass = true;
        } else if (dp && !plugin->hasPrecision(ProcessPrecision::Double)){ // possible
            dp = false;
        } else if (!dp && !plugin->hasPrecision(ProcessPrecision::Single)){ // pretty unlikely...
            dp = true;
        }
    }
    if (dp){ // double precision
        vstplugin_doperform<double>(x, n, bypass);
    } else { // single precision
        vstplugin_doperform<float>(x, n, bypass);
    }

    return (w+3);
}

// save function
static void vstplugin_save(t_gobj *z, t_binbuf *bb){
    t_vstplugin *x = (t_vstplugin *)z;
    binbuf_addv(bb, "ssff", &s__X, gensym("obj"),
        (float)x->x_obj.te_xpix, (float)x->x_obj.te_ypix);
    binbuf_addbinbuf(bb, x->x_obj.ob_binbuf);
    binbuf_addsemi(bb);
    if (x->x_keep && x->x_plugin){
            // 1) precision
        binbuf_addv(bb, "sss", gensym("#A"), gensym("precision"), gensym(x->x_dp ? "double" : "single"));
        binbuf_addsemi(bb);
            // 2) plugin
        if (x->x_editor->vst_gui()){
            binbuf_addv(bb, "ssss", gensym("#A"), gensym("open"), gensym("-e"), x->x_path);
        } else {
            binbuf_addv(bb, "sss", gensym("#A"), gensym("open"), x->x_path);
        }
        binbuf_addsemi(bb);
            // 3) program number
        binbuf_addv(bb, "ssi", gensym("#A"), gensym("program_set"), x->x_plugin->getProgram());
        binbuf_addsemi(bb);
            // 4) program data
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
    dsp_add(vstplugin_perform, 2, x, blocksize);
    if (blocksize != x->x_blocksize){
        x->x_blocksize = blocksize;
        x->update_buffer();
    }
    x->x_sr = sr;
    if (x->x_plugin){
        x->x_plugin->suspend();
        x->x_plugin->setBlockSize(blocksize);
        x->x_plugin->setSampleRate(sr);
        x->x_plugin->resume();
    }
    int nin = x->x_siginlets.size();
    int nout = x->x_sigoutlets.size();
    for (int i = 0; i < nin; ++i){
        x->x_siginlets[i] = sp[i]->s_vec;
    }
    for (int i = 0; i < nout; ++i){
        x->x_sigoutlets[i] = sp[nin + i]->s_vec;
    }
    // LOG_DEBUG("vstplugin~: got 'dsp' message");
}

// setup function
extern "C" {

void vstplugin_tilde_setup(void)
{
    vstplugin_class = class_new(gensym("vstplugin~"), (t_newmethod)(void *)vstplugin_new,
        (t_method)vstplugin_free, sizeof(t_vstplugin), 0, A_GIMME, A_NULL);
    CLASS_MAINSIGNALIN(vstplugin_class, t_vstplugin, x_f);
    class_setsavefn(vstplugin_class, vstplugin_save);
    class_addmethod(vstplugin_class, (t_method)vstplugin_dsp, gensym("dsp"), A_CANT, A_NULL);
        // plugin
    class_addmethod(vstplugin_class, (t_method)vstplugin_open, gensym("open"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_close, gensym("close"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_search, gensym("search"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_search_clear, gensym("search_clear"), A_DEFFLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_bypass, gensym("bypass"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_reset, gensym("reset"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_vis, gensym("vis"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_click, gensym("click"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_precision, gensym("precision"), A_SYMBOL, A_NULL);
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

    vstparam_setup();

    // read cached plugin info
    readIniFile();

#if !VSTTHREADS
    eventLoopClock = clock_new(0, (t_method)eventLoopTick);
    clock_delay(eventLoopClock, 0);
#endif
}

} // extern "C"
