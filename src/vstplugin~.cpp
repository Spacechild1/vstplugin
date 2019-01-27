#include "vstplugin~.h"

#undef pd_class
#define pd_class(x) (*(t_pd *)(x))
#define classname(x) (class_getname(pd_class(x)))

#if !VSTTHREADS // don't use VST GUI threads
# define MAIN_LOOP_POLL_INT 20
static t_clock *mainLoopClock = nullptr;
static void mainLoopTick(void *x){
    VSTWindowFactory::mainLoopPoll();
    clock_delay(mainLoopClock, MAIN_LOOP_POLL_INT);
}
#endif

// substitute SPACE for NO-BREAK SPACE (e.g. to avoid Tcl errors in the properties dialog)
static void substitute_whitespace(char *buf){
    for (char *c = buf; *c; c++){
        if (*c == ' '){
            *c = 160;
        }
    }
}

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
    IVSTPlugin *plugin = x->p_owner->x_plugin;
    int index = x->p_index;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", plugin->getParameterDisplay(index).c_str());
    pd_vmess(x->p_display_rcv->s_thing, gensym("set"), (char *)"s", gensym(buf));
}

static void vstparam_setup(){
    vstparam_class = class_new(gensym("__vstparam"), 0, 0, sizeof(t_vstparam), 0, A_NULL);
    class_addfloat(vstparam_class, (t_method)vstparam_float);
    class_addsymbol(vstparam_class, (t_method)vstparam_symbol);
    class_addmethod(vstparam_class, (t_method)vstparam_set, gensym("set"), A_DEFFLOAT, 0);
}

/*-------------------- t_vsteditor ------------------------*/

t_vsteditor::t_vsteditor(t_vstplugin &owner, t_gui gui)
    : e_owner(&owner){
#if VSTTHREADS
    e_mainthread = std::this_thread::get_id();
#endif
    if (gui != NO_GUI){
        pd_vmess(&pd_canvasmaker, gensym("canvas"), (char *)"iiiii", 0, 0, 100, 100, 10);
        e_canvas = (t_canvas *)s__X.s_thing;
        send_vmess(gensym("pop"), "i", 0);
    }
        // initialize GUI backend (if needed)
    if (gui == VST_GUI){
        static bool initialized = false;
        if (!initialized){
            VSTWindowFactory::initialize();
            initialized = true;
        }
    }
    e_gui = gui;
    e_clock = clock_new(this, (t_method)tick);
}

t_vsteditor::~t_vsteditor(){
    clock_free(e_clock);
}

    // post outgoing event (thread-safe if needed)
template<typename T, typename U>
void t_vsteditor::post_event(T& queue, U&& event){
#if VSTTHREADS
        // we only need to lock for GUI windows, but never for the generic Pd editor
    if (e_window){
        e_mutex.lock();
    }
#endif
    queue.push_back(std::forward<U>(event));
#if VSTTHREADS
    if (e_window){
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
void t_vsteditor::midiEvent(const VSTMidiEvent &event){
    post_event(e_midi, event);
}

void t_vsteditor::sysexEvent(const VSTSysexEvent &event){
    post_event(e_sysex, event);
}

void t_vsteditor::tick(t_vsteditor *x){
    t_outlet *outlet = x->e_owner->x_messout;
#if VSTTHREADS
        // we only need to lock for GUI windows, but never for the generic Pd editor
    if (x->e_window){
        x->e_mutex.lock();
    }
#endif
        // automated parameters:
    for (auto& param : x->e_automated){
        t_atom msg[2];
        SETFLOAT(&msg[0], param.first); // index
        SETFLOAT(&msg[1], param.second); // value
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
        int n = sysex.data.size();
        msg.resize(n);
        for (int i = 0; i < n; ++i){
            SETFLOAT(&msg[i], (unsigned char)sysex.data[i]);
        }
        outlet_anything(outlet, gensym("midi"), n, msg.data());
    }
    x->e_sysex.clear();
#if VSTTHREADS
    if (x->e_window){
        x->e_mutex.unlock();
    }
#endif
}

#if VSTTHREADS
    // create plugin + editor GUI (in another thread)
void t_vsteditor::thread_function(std::promise<IVSTPlugin *> promise, const char *path){
    LOG_DEBUG("enter thread");
    IVSTPlugin *plugin = loadVSTPlugin(path);
    if (!plugin){
            // signal main thread
        promise.set_value(nullptr);
        LOG_DEBUG("exit thread");
        return;
    }
        // create GUI window (if needed)
    if (plugin->hasEditor()){
        e_window = std::unique_ptr<IVSTWindow>(VSTWindowFactory::create(plugin));
    }
        // receive events from plugin
    plugin->setListener(this);
        // return plugin to main thread
    promise.set_value(plugin);
        // setup GUI window (if any)
    if (e_window){
        e_window->setTitle(plugin->getPluginName());
        int left, top, right, bottom;
        plugin->getEditorRect(left, top, right, bottom);
        e_window->setGeometry(left, top, right, bottom);

        plugin->openEditor(e_window->getHandle());

        LOG_DEBUG("enter message loop");
            // run the event loop until it gets a quit message
            // (the editor will we closed implicitly)
        e_window->run();
        LOG_DEBUG("exit message loop");
            // some plugins expect to released in the same thread where they have been created
        freeVSTPlugin(plugin);
        e_owner->x_plugin = nullptr;
        LOG_DEBUG("VST plugin closed");
    }
    LOG_DEBUG("exit thread");
}
#endif

IVSTPlugin* t_vsteditor::open_plugin(const char *path){
#if VSTTHREADS
        // creates a new thread where the plugin is created and the message loop runs
    if (e_gui == VST_GUI){
        std::promise<IVSTPlugin *> promise;
        auto future = promise.get_future();
        e_thread = std::thread(&t_vsteditor::thread_function, this, std::move(promise), path);
            // wait for thread to return the plugin
        return future.get();
    }
#endif
        // create plugin in main thread
    IVSTPlugin *plugin = loadVSTPlugin(path);
    if (!plugin){
        return nullptr;
    }
        // receive events from plugin
    plugin->setListener(this);
#if !VSTTHREADS
        // create and setup GUI window in main thread (if needed)
    if (plugin->hasEditor() && e_gui == VST_GUI){
        e_window = std::unique_ptr<IVSTWindow>(VSTWindowFactory::create(plugin));
        if (e_window){
            e_window->setTitle(plugin->getPluginName());
            int left, top, right, bottom;
            plugin->getEditorRect(left, top, right, bottom);
            e_window->setGeometry(left, top, right, bottom);
            // don't open the editor on macOS (see VSTWindowCocoa.mm)
#ifndef __APPLE__
            plugin->openEditor(e_window->getHandle());
#endif
        }
    }
#endif
    return plugin;
}

void t_vsteditor::close_plugin(){
    if (e_window){
        // terminate the message loop (if any) -
        // this will implicitly release the plugin
        e_window->quit();
    } else {
        vis(0); // close the Pd editor
    }
#if VSTTHREADS
        // now join the thread (if any)
    if (e_thread.joinable()){
        e_thread.join();
    }
#endif
        // now delete the window (if any)
    e_window = nullptr;
        // do we still have a plugin? (e.g. Pd editor or !VSTTHREADS)
    if (e_owner->x_plugin){
        freeVSTPlugin(e_owner->x_plugin);
        e_owner->x_plugin = nullptr;
        LOG_DEBUG("VST plugin closed");
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
    e_params.reserve(nparams);
    for (int i = 0; i < nparams; ++i){
        e_params.emplace_back(e_owner, i);
    }
        // slider: #X obj 25 43 hsl 128 15 0 1 0 0 snd rcv label -2 -8 0 10 -262144 -1 -1 0 1;
    t_atom slider[21];
    SETFLOAT(slider, 0); // temp
    SETFLOAT(slider+1, 0); // temp
    SETSYMBOL(slider+2, gensym("hsl"));
    SETFLOAT(slider+3, 128);
    SETFLOAT(slider+4, 15);
    SETFLOAT(slider+5, 0);
    SETFLOAT(slider+6, 1);
    SETFLOAT(slider+7, 0);
    SETFLOAT(slider+8, 0);
    SETSYMBOL(slider+9, gensym("snd")); // temp
    SETSYMBOL(slider+10, gensym("rcv")); // temp
    SETSYMBOL(slider+11, gensym("label")); // temp
    SETFLOAT(slider+12, -2);
    SETFLOAT(slider+13, -8);
    SETFLOAT(slider+14, 0);
    SETFLOAT(slider+15, 10);
    SETFLOAT(slider+16, -262144);
    SETFLOAT(slider+17, -1);
    SETFLOAT(slider+18, -1);
    SETFLOAT(slider+19, -0);
    SETFLOAT(slider+20, 1);
        // display: #X symbolatom 165 79 10 0 0 1 label rcv snd, f 10;
    t_atom display[9];
    SETFLOAT(display, 0); // temp
    SETFLOAT(display+1,0); // temp
    SETFLOAT(display+2, 10);
    SETFLOAT(display+3, 0);
    SETFLOAT(display+4, 0);
    SETFLOAT(display+5, 1);
    SETSYMBOL(display+6, gensym("")); // temp
    SETSYMBOL(display+7, gensym("rcv")); // temp
    SETSYMBOL(display+8, gensym("snd")); // temp

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
}

void t_vsteditor::update(){
    if (!e_owner->check_plugin()) return;

    if (e_window){
        e_window->update();
    } else if (pd_gui()) {
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
    if (e_window){
        if (v){
            e_window->bringToTop();
        } else {
            e_window->hide();
        }
    } else if (pd_gui()) {
        send_vmess(gensym("vis"), "i", (int)v);
    }
}

/*---------------- t_vstplugin (public methods) ------------------*/

// close
static void vstplugin_close(t_vstplugin *x){
    x->x_editor->close_plugin();
}

// open
static void vstplugin_open(t_vstplugin *x, t_symbol *s){
    vstplugin_close(x);
    char dirresult[MAXPDSTRING];
    char *name;
    std::string vstpath = makeVSTPluginFilePath(s->s_name);
#ifdef __APPLE__
    const char *bundleinfo = "/Contents/Info.plist";
    vstpath += bundleinfo; // on MacOS VSTs are bundles but canvas_open needs a real file
#endif
    int fd = canvas_open(x->x_canvas, vstpath.c_str(), "", dirresult, &name, MAXPDSTRING, 1);
    if (fd >= 0){
        sys_close(fd);
        char path[MAXPDSTRING];
        snprintf(path, MAXPDSTRING, "%s/%s", dirresult, name);
#ifdef __APPLE__
        char *find = strstr(path, bundleinfo);
        if (find){
            *find = 0; // restore the bundle path
        }
#endif
        sys_bashfilename(path, path);
            // load VST plugin in new thread
        IVSTPlugin *plugin = x->x_editor->open_plugin(path);
        if (plugin){
            post("loaded VST plugin '%s'", plugin->getPluginName().c_str());
                // initially, blocksize is 0 (before the 'dsp' message is sent).
                // some plugins might not like 0, so we send some sane default size.
            plugin->setBlockSize(x->x_blocksize > 0 ? x->x_blocksize : 64);
            plugin->setSampleRate(x->x_sr);
            x->x_plugin = plugin;
            x->update_precision();
            x->update_buffer();
            x->x_editor->setup();
        } else {
            pd_error(x, "%s: couldn't open \"%s\" - not a VST plugin!", classname(x), path);
        }
    } else {
        pd_error(x, "%s: couldn't open \"%s\" - no such file!", classname(x), s->s_name);
    }
}

// plugin name
static void vstplugin_name(t_vstplugin *x){
    if (!x->check_plugin()) return;
    t_symbol *name = gensym(x->x_plugin->getPluginName().c_str());
    t_atom msg;
    SETSYMBOL(&msg, name);
    outlet_anything(x->x_messout, gensym("name"), 1, &msg);
}

// plugin version
static void vstplugin_version(t_vstplugin *x){
    if (!x->check_plugin()) return;
    int version = x->x_plugin->getPluginVersion();
    t_atom msg;
    SETFLOAT(&msg, version);
    outlet_anything(x->x_messout, gensym("version"), 1, &msg);
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

// vendor specific action
static void vstplugin_vendor_method(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    int index = atom_getfloatarg(0, argc, argv);
    intptr_t value = atom_getfloatarg(1, argc, argv);
    float opt = atom_getfloatarg(2, argc, argv);
    char *data = nullptr;
    int size = argc - 3;
    if (size > 0){
        data = (char *)getbytes(size);
        for (int i = 0, j = 3; i < size; ++i, ++j){
            data[i] = atom_getfloat(argv + j);
        }
    }
    int result = x->x_plugin->vedorSpecific(index, value, data, opt);
    t_atom msg;
    SETFLOAT(&msg, result);
    outlet_anything(x->x_messout, gensym("vendor_method"), 1, &msg);
    if (data){
        freebytes(data, size);
    }
}

// print plugin info in Pd console
static void vstplugin_info(t_vstplugin *x){
    if (!x->check_plugin()) return;
    post("~~~ VST plugin info ~~~");
    post("name: %s", x->x_plugin->getPluginName().c_str());
    post("version: %d", x->x_plugin->getPluginVersion());
    post("input channels: %d", x->x_plugin->getNumInputs());
    post("output channels: %d", x->x_plugin->getNumOutputs());
    post("single precision: %s", x->x_plugin->hasPrecision(VSTProcessPrecision::Single) ? "yes" : "no");
    post("double precision: %s", x->x_plugin->hasPrecision(VSTProcessPrecision::Double) ? "yes" : "no");
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

// set parameter by float (0.0 - 1.0) or string (if supported)
static void vstplugin_param_set(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    if (argc < 2){
        pd_error(x, "%s: 'param_set' expects two arguments (index + float/symbol)", classname(x));
    }
    int index = atom_getfloat(argv);
    switch (argv[1].a_type){
    case A_FLOAT:
        x->set_param(index, argv[1].a_w.w_float, false);
        break;
    case A_SYMBOL:
        x->set_param(index, argv[1].a_w.w_symbol->s_name, false);
        break;
    default:
        pd_error(x, "%s: second argument for 'param_set' must be a float or symbol", classname(x));
        break;
    }
}

// get parameter value
static void vstplugin_param_get(t_vstplugin *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
		t_atom msg[2];
		SETFLOAT(&msg[0], index);
        SETFLOAT(&msg[1], x->x_plugin->getParameter(index));
		outlet_anything(x->x_messout, gensym("param_value"), 2, msg);
	} else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
	}
}

// get parameter name
static void vstplugin_param_name(t_vstplugin *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
		t_atom msg[2];
		SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getParameterName(index).c_str()));
		outlet_anything(x->x_messout, gensym("param_name"), 2, msg);
	} else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
	}
}

// get parameter label (unit of measurement, e.g. ms or dB)
static void vstplugin_param_label(t_vstplugin *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
        t_atom msg[2];
        SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getParameterLabel(index).c_str()));
        outlet_anything(x->x_messout, gensym("param_label"), 2, msg);
    } else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
    }
}

// get stringified parameter value
static void vstplugin_param_display(t_vstplugin *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
        t_atom msg[2];
        SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getParameterDisplay(index).c_str()));
        outlet_anything(x->x_messout, gensym("param_display"), 2, msg);
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

// list parameters (index + name)
static void vstplugin_param_list(t_vstplugin *x){
    if (!x->check_plugin()) return;
    int n = x->x_plugin->getNumParameters();
	for (int i = 0; i < n; ++i){
        vstplugin_param_name(x, i);
	}
}

// list parameter states (index + value)
static void vstplugin_param_dump(t_vstplugin *x){
    if (!x->check_plugin()) return;
    int n = x->x_plugin->getNumParameters();
    for (int i = 0; i < n; ++i){
        vstplugin_param_get(x, i);
    }
}

/*------------------------------------- MIDI -----------------------------------------*/

// send raw MIDI message
static void vstplugin_midi_raw(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;

    VSTMidiEvent event;
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

static void vstplugin_midi_aftertouch(t_vstplugin *x, t_floatarg channel, t_floatarg pitch, t_floatarg pressure){
    vstplugin_midi_mess(x, 160, channel, pitch, pressure);
}

static void vstplugin_midi_cc(t_vstplugin *x, t_floatarg channel, t_floatarg ctl, t_floatarg value){
    vstplugin_midi_mess(x, 176, channel, ctl, value);
}

static void vstplugin_midi_program_change(t_vstplugin *x, t_floatarg channel, t_floatarg program){
   vstplugin_midi_mess(x, 192, channel, program);
}

static void vstplugin_midi_channel_aftertouch(t_vstplugin *x, t_floatarg channel, t_floatarg pressure){
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

    x->x_plugin->sendSysexEvent(VSTSysexEvent(std::move(data)));
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

// set program data (list of bytes)
static void vstplugin_program_data_set(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    std::string buffer;
    buffer.resize(argc);
    for (int i = 0; i < argc; ++i){
           // first clamp to 0-255, then assign to char (not 100% portable...)
        buffer[i] = (unsigned char)atom_getfloat(argv + i);
    }
    if (x->x_plugin->readProgramData(buffer)){
        x->x_editor->update();
    } else {
        pd_error(x, "%s: bad FX program data", classname(x));
    }
}

// get program data
static void vstplugin_program_data_get(t_vstplugin *x){
    if (!x->check_plugin()) return;
    std::string buffer;
    x->x_plugin->writeProgramData(buffer);
    int n = buffer.size();
    if (!n){
        pd_error(x, "%s: couldn't get program data", classname(x));
        return;
    }
    std::vector<t_atom> atoms;
    atoms.resize(n);
    for (int i = 0; i < n; ++i){
            // first convert to range 0-255, then assign to t_float (not 100% portable...)
        SETFLOAT(&atoms[i], (unsigned char)buffer[i]);
    }
    outlet_anything(x->x_messout, gensym("program_data"), n, atoms.data());
}

// read program file (.FXP)
static void vstplugin_program_read(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char dir[MAXPDSTRING], *name;
    int fd = canvas_open(x->x_canvas, s->s_name, "", dir, &name, MAXPDSTRING, 1);
    if (fd < 0){
        pd_error(x, "%s: couldn't find file '%s'", classname(x), s->s_name);
        return;
    }
    sys_close(fd);
    char path[MAXPDSTRING];
    snprintf(path, MAXPDSTRING, "%s/%s", dir, name);
    sys_bashfilename(path, path);
    if (x->x_plugin->readProgramFile(path)){
        x->x_editor->update();
    } else {
        pd_error(x, "%s: bad FX program file '%s'", classname(x), s->s_name);
    }
}

// write program file (.FXP)
static void vstplugin_program_write(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char path[MAXPDSTRING];
    canvas_makefilename(x->x_canvas, s->s_name, path, MAXPDSTRING);
    x->x_plugin->writeProgramFile(path);
}

// set bank data (list of bytes)
static void vstplugin_bank_data_set(t_vstplugin *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;
    std::string buffer;
    buffer.resize(argc);
    for (int i = 0; i < argc; ++i){
            // first clamp to 0-255, then assign to char (not 100% portable...)
        buffer[i] = (unsigned char)atom_getfloat(argv + i);
    }
    if (x->x_plugin->readBankData(buffer)){
        x->x_editor->update();
    } else {
        pd_error(x, "%s: bad FX bank data", classname(x));
    }
}

// get bank data
static void vstplugin_bank_data_get(t_vstplugin *x){
    if (!x->check_plugin()) return;
    std::string buffer;
    x->x_plugin->writeBankData(buffer);
    int n = buffer.size();
    if (!n){
        pd_error(x, "%s: couldn't get bank data", classname(x));
        return;
    }
    std::vector<t_atom> atoms;
    atoms.resize(n);
    for (int i = 0; i < n; ++i){
            // first convert to range 0-255, then assign to t_float (not 100% portable...)
        SETFLOAT(&atoms[i], (unsigned char)buffer[i]);
    }
    outlet_anything(x->x_messout, gensym("bank_data"), n, atoms.data());
}

// read bank file (.FXB)
static void vstplugin_bank_read(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char dir[MAXPDSTRING], *name;
    int fd = canvas_open(x->x_canvas, s->s_name, "", dir, &name, MAXPDSTRING, 1);
    if (fd < 0){
        pd_error(x, "%s: couldn't find file '%s'", classname(x), s->s_name);
        return;
    }
    sys_close(fd);
    char path[MAXPDSTRING];
    snprintf(path, MAXPDSTRING, "%s/%s", dir, name);
    sys_bashfilename(path, path);
    if (x->x_plugin->readBankFile(path)){
        x->x_editor->update();
    } else {
        pd_error(x, "%s: bad FX bank file '%s'", classname(x), s->s_name);
    }
}

// write bank file (.FXB)
static void vstplugin_bank_write(t_vstplugin *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char path[MAXPDSTRING];
    canvas_makefilename(x->x_canvas, s->s_name, path, MAXPDSTRING);
    x->x_plugin->writeBankFile(path);
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
        if (!x_plugin->hasPrecision(VSTProcessPrecision::Single) && !x_plugin->hasPrecision(VSTProcessPrecision::Double)) {
            post("%s: '%s' doesn't support single or double precision, bypassing",
                classname(this), x_plugin->getPluginName().c_str());
            return;
        }
        if (x_dp && !x_plugin->hasPrecision(VSTProcessPrecision::Double)){
            post("%s: '%s' doesn't support double precision, using single precision instead",
                 classname(this), x_plugin->getPluginName().c_str());
            dp = 0;
        }
        else if (!x_dp && !x_plugin->hasPrecision(VSTProcessPrecision::Single)){ // very unlikely...
            post("%s: '%s' doesn't support single precision, using double precision instead",
                 classname(this), x_plugin->getPluginName().c_str());
            dp = 1;
        }
            // set the actual precision
        x_plugin->setPrecision(dp ? VSTProcessPrecision::Double : VSTProcessPrecision::Single);
    }
}

// constructor
// usage: vstplugin~ [flags...] [file] inlets (default=2) outlets (default=2)
t_vstplugin::t_vstplugin(int argc, t_atom *argv){
#ifdef __APPLE__
    t_gui gui = PD_GUI; // use generic Pd GUI by default
#else
    t_gui gui = VST_GUI; // use VST GUI by default
#endif
    int dp = (PD_FLOATSIZE == 64); // use double precision? default to precision of Pd
    t_symbol *file = nullptr; // plugin to load (optional)

    while (argc && argv->a_type == A_SYMBOL){
        const char *flag = atom_getsymbol(argv)->s_name;
        if (*flag == '-'){
            if (!strcmp(flag, "-vstgui")){
                gui = VST_GUI;
            } else if (!strcmp(flag, "-pdgui")){
                gui = PD_GUI;
            } else if (!strcmp(flag, "-nogui")){
                gui = NO_GUI;
            } else if (!strcmp(flag, "-sp")){
                dp = 0;
            } else if (!strcmp(flag, "-dp")){
                dp = 1;
            }
            argc--; argv++;
        } else {
            file = argv->a_w.w_symbol;
            argc--; argv++;
            break;
        }
    }
    int in = atom_getfloatarg(0, argc, argv); // signal inlets
    int out = atom_getfloatarg(1, argc, argv); // signal outlets
    if (in < 1) in = 2;
    if (out < 1) out = 2;

    x_dp = dp;
    x_canvas = canvas_getcurrent();
    x_editor = std::unique_ptr<t_vsteditor>(new t_vsteditor(*this, gui)); // C++11

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

    if (file){
        vstplugin_open(this, file);
    }
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
    auto plugin = x->x_plugin;

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
    auto plugin = x->x_plugin;
    bool dp = x->x_dp;
    bool bypass = plugin ? x->x_bypass : true;

    if (plugin && !bypass) {
            // check processing precision (single or double)
        if (!plugin->hasPrecision(VSTProcessPrecision::Single)
                && !plugin->hasPrecision(VSTProcessPrecision::Double)) { // very unlikely...
            bypass = true;
        } else if (dp && !plugin->hasPrecision(VSTProcessPrecision::Double)){ // possible
            dp = false;
        } else if (!dp && !plugin->hasPrecision(VSTProcessPrecision::Single)){ // pretty unlikely...
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
    vstplugin_class = class_new(gensym("vstplugin~"), (t_newmethod)vstplugin_new, (t_method)vstplugin_free,
        sizeof(t_vstplugin), 0, A_GIMME, A_NULL);
    CLASS_MAINSIGNALIN(vstplugin_class, t_vstplugin, x_f);
    class_addmethod(vstplugin_class, (t_method)vstplugin_dsp, gensym("dsp"), A_CANT, A_NULL);
        // plugin
    class_addmethod(vstplugin_class, (t_method)vstplugin_open, gensym("open"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_close, gensym("close"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_bypass, gensym("bypass"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_reset, gensym("reset"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_vis, gensym("vis"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_click, gensym("click"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_precision, gensym("precision"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_name, gensym("name"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_version, gensym("version"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_can_do, gensym("can_do"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_vendor_method, gensym("vendor_method"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_info, gensym("info"), A_NULL);
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
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_get, gensym("param_get"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_name, gensym("param_name"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_label, gensym("param_label"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_display, gensym("param_display"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_count, gensym("param_count"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_list, gensym("param_list"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_param_dump, gensym("param_dump"), A_NULL);
        // midi
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_raw, gensym("midi_raw"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_note, gensym("midi_note"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_noteoff, gensym("midi_noteoff"), A_FLOAT, A_FLOAT, A_DEFFLOAT, A_NULL); // third floatarg is optional!
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_cc, gensym("midi_cc"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_bend, gensym("midi_bend"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_program_change, gensym("midi_program_change"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_aftertouch, gensym("midi_aftertouch"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_channel_aftertouch, gensym("midi_channel_aftertouch"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_midi_sysex, gensym("midi_sysex"), A_GIMME, A_NULL);
        // programs
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_set, gensym("program_set"), A_FLOAT, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_get, gensym("program_get"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_name_set, gensym("program_name_set"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_name_get, gensym("program_name_get"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_count, gensym("program_count"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_list, gensym("program_list"), A_NULL);
        // read/write fx programs
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_data_set, gensym("program_data_set"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_data_get, gensym("program_data_get"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_read, gensym("program_read"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_program_write, gensym("program_write"), A_SYMBOL, A_NULL);
        // read/write fx banks
    class_addmethod(vstplugin_class, (t_method)vstplugin_bank_data_set, gensym("bank_data_set"), A_GIMME, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_bank_data_get, gensym("bank_data_get"), A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_bank_read, gensym("bank_read"), A_SYMBOL, A_NULL);
    class_addmethod(vstplugin_class, (t_method)vstplugin_bank_write, gensym("bank_write"), A_SYMBOL, A_NULL);

    vstparam_setup();

#if !VSTTHREADS
    mainLoopClock = clock_new(0, (t_method)mainLoopTick);
    clock_delay(mainLoopClock, 0);
#endif
}

} // extern "C"
