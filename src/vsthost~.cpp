#include "VSTPluginInterface.h"
#include "Utility.h"

#include "m_pd.h"

#include <memory>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <future>
#include <iostream>
#include <vector>

#ifdef USE_X11
# include <X11/Xlib.h>
#endif

#undef pd_class
#define pd_class(x) (*(t_pd *)(x))
#define classname(x) (class_getname(pd_class(x)))

// vsthost~ object
static t_class *vsthost_class;

class t_vsteditor;

struct t_vsthost {
	t_object x_obj;
	t_sample x_f;
    t_outlet *x_messout;
        // VST plugin
    IVSTPlugin* x_plugin;
    int x_bypass;
    int x_blocksize;
    int x_sr;
    int x_dp; // use double precision
        // editor
    t_vsteditor *x_editor;
        // input signals from Pd
    int x_nin;
    t_float **x_invec;
        // contiguous input buffer
    int x_inbufsize;
    char *x_inbuf;
        // array of pointers into the input buffer
    int x_ninbuf;
    void **x_inbufvec;
        // output signals from Pd
    int x_nout;
    t_float **x_outvec;
        // contiguous output buffer
    int x_outbufsize;
    char *x_outbuf;
        // array of pointers into the output buffer
    int x_noutbuf;
    void **x_outbufvec;
        // helper methods
    bool check_plugin();
    void check_precision();
    void update_buffer();
};


// VST parameter responder (for Pd GUI)
static t_class *vstparam_class;

class t_vstparam {
 public:
    t_vstparam(t_vsthost *x, int index)
        : p_owner(x), p_index(index){
        p_pd = vstparam_class;
        char buf[64];
        snprintf(buf, sizeof(buf), "%p-hsl-%d", x, index);
        p_name = gensym(buf);
        pd_bind(&p_pd, p_name);
            // post("parameter nr. %d: %s", index, x->p_name->s_name);
        snprintf(buf, sizeof(buf), "%p-cnv-%d", x, index);
        p_display = gensym(buf);
    }
    ~t_vstparam(){
        pd_unbind(&p_pd, p_name);
    }
        // this will set the slider and implicitly call vstparam_set
    void set(t_floatarg f){
        pd_vmess(p_name->s_thing, gensym("set"), (char *)"f", f);
    }

    t_pd p_pd;
    t_vsthost *p_owner;
    t_symbol *p_name;
    t_symbol *p_display;
    int p_index;
};

static void vsthost_param_doset(t_vsthost *x, int index, float value, bool automated);

    // called when moving a slider in the generic GUI
static void vstparam_float(t_vstparam *x, t_floatarg f){
    vsthost_param_doset(x->p_owner, x->p_index, f, true);
}

static void vstparam_set(t_vstparam *x, t_floatarg f){
        // this method updates the display next to the label. implicitly called by t_vstparam::set
    IVSTPlugin *plugin = x->p_owner->x_plugin;
    int index = x->p_index;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %s", plugin->getParameterDisplay(index).c_str(),
        plugin->getParameterLabel(index).c_str());
    pd_vmess(x->p_display->s_thing, gensym("label"), (char *)"s", gensym(buf));
}

static void vstparam_setup(){
    vstparam_class = class_new(gensym("__vstparam"), 0, 0, sizeof(t_vstparam), 0, A_NULL);
    class_addfloat(vstparam_class, (t_method)vstparam_float);
    class_addmethod(vstparam_class, (t_method)vstparam_set, gensym("set"), A_DEFFLOAT, 0);
}

// VST editor
class t_vsteditor : IVSTPluginListener {
 public:
    t_vsteditor(t_vsthost &owner, bool generic);
    ~t_vsteditor();
        // try to open the plugin in a new thread and start the message loop (if needed)
    IVSTPlugin* open_via_thread(const char* path);
        // terminate the message loop and wait for the thread to finish
    void close_thread();
        // setup the generic Pd editor
    void setup();
        // update the parameter displays
    void update();
        // notify generic GUI for parameter changes
    void set_param(int index, float value, bool automated = false);
        // show/hide window
    void vis(bool v);
    IVSTWindow *window(){
        return e_window.get();
    }
    t_canvas *canvas(){
        return e_canvas;
    }
 private:
        // plugin callbacks
    void parameterAutomated(int index, float value) override;
    void midiEvent(const VSTMidiEvent& event) override;
    void sysexEvent(const VSTSysexEvent& event) override;
        // helper functions
    void send_mess(t_symbol *sel, int argc = 0, t_atom *argv = 0){
        pd_typedmess((t_pd *)e_canvas, sel, argc, argv);
    }
    template<typename... T>
    void send_vmess(t_symbol *sel, const char *fmt, T... args){
        pd_vmess((t_pd *)e_canvas, sel, (char *)fmt, args...);
    }
    void thread_function(std::promise<IVSTPlugin *> promise, const char *path);
    static void tick(t_vsteditor *x);
        // data
    t_vsthost *e_owner;
    std::thread e_thread;
    std::unique_ptr<IVSTWindow> e_window;
    bool e_generic = false;
    t_canvas *e_canvas = nullptr;
    void *e_context = nullptr;
    std::vector<t_vstparam> e_params;
        // message out
    t_clock *e_clock;
    std::mutex e_mutex;
    std::vector<std::pair<int, float>> e_automated;
    std::vector<VSTMidiEvent> e_midi;
    std::vector<VSTSysexEvent> e_sysex;
};

t_vsteditor::t_vsteditor(t_vsthost &owner, bool generic)
    : e_owner(&owner), e_generic(generic){
#if USE_X11
	if (!generic){
		e_context = XOpenDisplay(NULL);
		if (!e_context){
            LOG_WARNING("couldn't open display");
		}
	}
#endif
    glob_setfilename(0, gensym("VST Plugin Editor"), canvas_getcurrentdir());
    pd_vmess(&pd_canvasmaker, gensym("canvas"), (char *)"siiiii", 0, 0, 100, 100, 10);
    e_canvas = (t_canvas *)s__X.s_thing;
    send_vmess(gensym("pop"), "i", 0);
    glob_setfilename(0, &s_, &s_);

    e_clock = clock_new(this, (t_method)tick);
}

t_vsteditor::~t_vsteditor(){
#ifdef USE_X11
	if (e_context){
		XCloseDisplay((Display *)e_context);
	}
#endif
    clock_free(e_clock);
}

    // parameter automation notification will most likely come from the GUI thread
void t_vsteditor::parameterAutomated(int index, float value){
    e_mutex.lock();
    e_automated.push_back(std::make_pair(index, value));
    e_mutex.unlock();
    sys_lock();
    clock_delay(e_clock, 0);
    sys_unlock();
}

    // MIDI and SysEx events should only be called from the audio thread
void t_vsteditor::midiEvent(const VSTMidiEvent &event){
    e_midi.push_back(event);
    clock_delay(e_clock, 0);
}

void t_vsteditor::sysexEvent(const VSTSysexEvent &event){
    e_sysex.push_back(event);
    clock_delay(e_clock, 0);
}

void t_vsteditor::tick(t_vsteditor *x){
    t_outlet *outlet = x->e_owner->x_messout;
        // automated parameters (needs thread synchronization):
    x->e_mutex.lock();
    for (auto& param : x->e_automated){
        t_atom msg[2];
        SETFLOAT(&msg[0], param.first); // index
        SETFLOAT(&msg[1], param.second); // value
        outlet_anything(outlet, gensym("param_automated"), 2, msg);
    }
    x->e_automated.clear();
    x->e_mutex.unlock();
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
}

void t_vsteditor::thread_function(std::promise<IVSTPlugin *> promise, const char *path){
    LOG_DEBUG("enter thread");
    IVSTPlugin *plugin = loadVSTPlugin(path);
    if (!plugin){
        promise.set_value(nullptr);
        LOG_DEBUG("exit thread");
        return;
    }
    if (plugin->hasEditor() && !e_generic){
        e_window = std::unique_ptr<IVSTWindow>(VSTWindowFactory::create(e_context));
    }
    plugin->setListener(this);
    promise.set_value(plugin);
    if (e_window){
        e_window->setTitle(plugin->getPluginName());
        plugin->openEditor(e_window->getHandle());
        int left, top, right, bottom;
        plugin->getEditorRect(left, top, right, bottom);
        e_window->setGeometry(left, top, right, bottom);

        LOG_DEBUG("enter message loop");
        e_window->run();
        LOG_DEBUG("exit message loop");

        plugin->closeEditor();
            // some plugins expect to released in the same thread where they have been created
        LOG_DEBUG("try to close VST plugin");
        freeVSTPlugin(e_owner->x_plugin);
        e_owner->x_plugin = nullptr;
        LOG_DEBUG("VST plugin closed");
    }
    LOG_DEBUG("exit thread");
}

// creates a new thread where the plugin is created and the message loop runs (if needed)
IVSTPlugin* t_vsteditor::open_via_thread(const char *path){
    std::promise<IVSTPlugin *> promise;
    auto future = promise.get_future();
    e_thread = std::thread(&t_vsteditor::thread_function, this, std::move(promise), path);
    return future.get();
}

void t_vsteditor::close_thread(){
        // destroying the window will terminate the message loop and release the plugin
    e_window = nullptr;
        // now join the thread
    if (e_thread.joinable()){
        e_thread.join();
    }
}

void t_vsteditor::setup(){
    if (!e_owner->check_plugin()) return;

    int nparams = e_owner->x_plugin->getNumParameters();
    e_params.clear();
    e_params.reserve(nparams);
    for (int i = 0; i < nparams; ++i){
        e_params.emplace_back(e_owner, i);
    }
    send_vmess(gensym("rename"), (char *)"s", gensym(e_owner->x_plugin->getPluginName().c_str()));
    send_mess(gensym("clear"));
        // slider
    t_atom slider[21];
    SETFLOAT(slider, 20);
    SETFLOAT(slider+1, 20);
    SETSYMBOL(slider+2, gensym("hsl"));
    SETFLOAT(slider+3, 128);
    SETFLOAT(slider+4, 15);
    SETFLOAT(slider+5, 0);
    SETFLOAT(slider+6, 1);
    SETFLOAT(slider+7, 0);
    SETFLOAT(slider+8, 0);
    SETSYMBOL(slider+9, gensym("empty"));
    SETSYMBOL(slider+10, gensym("empty"));
    SETSYMBOL(slider+11, gensym("dummy"));
    SETFLOAT(slider+12, -2);
    SETFLOAT(slider+13, -8);
    SETFLOAT(slider+14, 0);
    SETFLOAT(slider+15, 10);
    SETFLOAT(slider+16, -262144);
    SETFLOAT(slider+17, -1);
    SETFLOAT(slider+18, -1);
    SETFLOAT(slider+19, -0);
    SETFLOAT(slider+20, 1);
        // label
    t_atom label[16];
    SETFLOAT(label, 30 + 128);
    SETFLOAT(label+1, 10);
    SETSYMBOL(label+2, gensym("cnv"));
    SETFLOAT(label+3, 1);
    SETFLOAT(label+4, 1);
    SETFLOAT(label+5, 1);
    SETSYMBOL(label+6, gensym("empty"));
    SETSYMBOL(label+7, gensym("empty"));
    SETSYMBOL(label+8, gensym("empty"));
    SETFLOAT(label+9, 0);
    SETFLOAT(label+10, 8);
    SETFLOAT(label+11, 0);
    SETFLOAT(label+12, 10);
    SETFLOAT(label+13, -262144);
    SETFLOAT(label+14, -66577);
    SETFLOAT(label+15, 0);

    for (int i = 0; i < nparams; ++i){
            // create slider
        SETFLOAT(slider+1, 20 + i*35);
        SETSYMBOL(slider+9, e_params[i].p_name);
        SETSYMBOL(slider+10, e_params[i].p_name);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d: %s", i, e_owner->x_plugin->getParameterName(i).c_str());
        SETSYMBOL(slider+11, gensym(buf));
        send_mess(gensym("obj"), 21, slider);
            // create number box
        SETFLOAT(label+1, 20 + i*35);
        SETSYMBOL(label+6, e_params[i].p_display);
        SETSYMBOL(label+7, e_params[i].p_display);
        send_mess(gensym("obj"), 16, label);
    }
    float width = 280;
    float height = nparams * 35 + 60;
    if (height > 800) height = 800;
    send_vmess(gensym("setbounds"), "ffff", 0.f, 0.f, width, height);
    send_vmess(gensym("vis"), "i", 0);

    update();
}

void t_vsteditor::update(){
    if (!e_owner->check_plugin()) return;

    if (!e_owner->x_plugin->hasEditor() || e_generic){
        int n = e_owner->x_plugin->getNumParameters();
        for (int i = 0; i < n; ++i){
            set_param(i, e_owner->x_plugin->getParameter(i));
        }
    }
}

    // automated: true if parameter change comes from the (generic) GUI
void t_vsteditor::set_param(int index, float value, bool automated){
    if (!e_window && index >= 0 && index < (int)e_params.size()){
        e_params[index].set(value);
        if (automated){
            e_automated.push_back(std::make_pair(index, value));
            clock_delay(e_clock, 0);
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
    } else {
        send_vmess(gensym("vis"), "i", (int)v);
    }
}

/**** public interface ****/

// close
static void vsthost_close(t_vsthost *x){
    if (x->x_plugin){
        x->x_editor->vis(0);
            // first close the thread (might already free the plugin)
        x->x_editor->close_thread();
            // then free the plugin if necessary
        if (x->x_plugin){
            LOG_DEBUG("try to close VST plugin");
            freeVSTPlugin(x->x_plugin);
            x->x_plugin = nullptr;
            LOG_DEBUG("VST plugin closed");
        }
    }
}

// open
static void vsthost_open(t_vsthost *x, t_symbol *s){
    vsthost_close(x);
    char dirresult[MAXPDSTRING];
    char *name;
    std::string vstpath = makeVSTPluginFilePath(s->s_name);
#ifdef __APPLE__
    const char *bundleinfo = "/Contents/Info.plist";
    vstpath += bundleinfo; // on MacOS VSTs are bundles but canvas_open needs a real file
#endif
    int fd = canvas_open(x->x_editor->canvas(), vstpath.c_str(), "", dirresult, &name, MAXPDSTRING, 1);
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
        IVSTPlugin *plugin = x->x_editor->open_via_thread(path);
        if (plugin){
            post("loaded VST plugin '%s'", plugin->getPluginName().c_str());
            plugin->setBlockSize(x->x_blocksize);
            plugin->setSampleRate(x->x_sr);
            x->x_plugin = plugin;
            x->check_precision();
            x->update_buffer();
            x->x_editor->setup();
        } else {
            pd_error(x, "%s: couldn't open \"%s\" - not a VST plugin!", classname(x), path);
        }
            // no window -> no message loop
        if (!x->x_editor->window()){
            x->x_editor->close_thread();
        }
    } else {
        pd_error(x, "%s: couldn't open \"%s\" - no such file!", classname(x), s->s_name);
    }
}

static void vsthost_info(t_vsthost *x){
    if (!x->check_plugin()) return;
    post("~~~ VST plugin info ~~~");
    post("name: %s", x->x_plugin->getPluginName().c_str());
    post("version: %d", x->x_plugin->getPluginVersion());
    post("input channels: %d", x->x_plugin->getNumInputs());
    post("output channels: %d", x->x_plugin->getNumOutputs());
    post("single precision: %s", x->x_plugin->hasSinglePrecision() ? "yes" : "no");
    post("double precision: %s", x->x_plugin->hasDoublePrecision() ? "yes" : "no");
    post("editor: %s", x->x_plugin->hasEditor() ? "yes" : "no");
    post("number of parameters: %d", x->x_plugin->getNumParameters());
    post("number of programs: %d", x->x_plugin->getNumPrograms());
    post("synth: %s", x->x_plugin->isSynth() ? "yes" : "no");
    post("midi input: %s", x->x_plugin->hasMidiInput() ? "yes" : "no");
    post("midi output: %s", x->x_plugin->hasMidiOutput() ? "yes" : "no");
    post("");
}

static void vsthost_bypass(t_vsthost *x, t_floatarg f){
    x->x_bypass = (f != 0);
    if (x->x_plugin){
        if (x->x_bypass){
            x->x_plugin->suspend();
        } else {
            x->x_plugin->resume();
        }
    }
}

static void vsthost_vis(t_vsthost *x, t_floatarg f){
    if (!x->check_plugin()) return;
    x->x_editor->vis(f);
}

static void vsthost_click(t_vsthost *x){
    vsthost_vis(x, 1);
}

static void vsthost_precision(t_vsthost *x, t_floatarg f){
    x->x_dp = (f != 0);
    x->check_precision();
}

// parameters
static void vsthost_param_set(t_vsthost *x, t_floatarg index, t_floatarg value){
    if (!x->check_plugin()) return;
    vsthost_param_doset(x, index, value, false);
}

    // automated: true if parameter was set from the (generic) GUI, false if set by message ("param_set")
static void vsthost_param_doset(t_vsthost *x, int index, float value, bool automated){
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
        value = std::max(0.f, std::min(1.f, value));
        x->x_plugin->setParameter(index, value);
        x->x_editor->set_param(index, value, automated);
    } else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
    }
}

static void vsthost_param_get(t_vsthost *x, t_floatarg _index){
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

static void vsthost_param_name(t_vsthost *x, t_floatarg _index){
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

static void vsthost_param_label(t_vsthost *x, t_floatarg _index){
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

static void vsthost_param_display(t_vsthost *x, t_floatarg _index){
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

static void vsthost_param_count(t_vsthost *x){
    if (!x->check_plugin()) return;
	t_atom msg;
    SETFLOAT(&msg, x->x_plugin->getNumParameters());
	outlet_anything(x->x_messout, gensym("param_count"), 1, &msg);
}

static void vsthost_param_list(t_vsthost *x){
    if (!x->check_plugin()) return;
    int n = x->x_plugin->getNumParameters();
	for (int i = 0; i < n; ++i){
        vsthost_param_name(x, i);
	}
}

static void vsthost_param_dump(t_vsthost *x){
    if (!x->check_plugin()) return;
    int n = x->x_plugin->getNumParameters();
    for (int i = 0; i < n; ++i){
        vsthost_param_get(x, i);
    }
}

// MIDI
static void vsthost_midi_raw(t_vsthost *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;

    VSTMidiEvent event;
    for (int i = 0; i < 3 && i < argc; ++i){
        event.data[i] = atom_getfloat(argv + i);
    }
    x->x_plugin->sendMidiEvent(event);
}

// helper function
static void vsthost_midi_mess(t_vsthost *x, int onset, int channel, int v1, int v2 = 0){
    t_atom atoms[3];
    channel = std::max(1, std::min(16, (int)channel)) - 1;
    v1 = std::max(0, std::min(127, v1));
    v2 = std::max(0, std::min(127, v2));
    SETFLOAT(&atoms[0], channel + onset);
    SETFLOAT(&atoms[1], v1);
    SETFLOAT(&atoms[2], v2);
    vsthost_midi_raw(x, 0, 3, atoms);
}

static void vsthost_midi_noteoff(t_vsthost *x, t_floatarg channel, t_floatarg pitch, t_floatarg velocity){
    vsthost_midi_mess(x, 128, channel, pitch, velocity);
}

static void vsthost_midi_note(t_vsthost *x, t_floatarg channel, t_floatarg pitch, t_floatarg velocity){
    vsthost_midi_mess(x, 144, channel, pitch, velocity);
}

static void vsthost_midi_aftertouch(t_vsthost *x, t_floatarg channel, t_floatarg pitch, t_floatarg pressure){
    vsthost_midi_mess(x, 160, channel, pitch, pressure);
}

static void vsthost_midi_cc(t_vsthost *x, t_floatarg channel, t_floatarg ctl, t_floatarg value){
    vsthost_midi_mess(x, 176, channel, ctl, value);
}

static void vsthost_midi_program_change(t_vsthost *x, t_floatarg channel, t_floatarg program){
   vsthost_midi_mess(x, 192, channel, program);
}

static void vsthost_midi_channel_aftertouch(t_vsthost *x, t_floatarg channel, t_floatarg pressure){
    vsthost_midi_mess(x, 208, channel, pressure);
}

static void vsthost_midi_bend(t_vsthost *x, t_floatarg channel, t_floatarg bend){
        // map from [-1.f, 1.f] to [0, 16383] (14 bit)
    int val = (bend + 1.f) * 8192.f; // 8192 is the center position
    val = std::max(0, std::min(16383, val));
    vsthost_midi_mess(x, 224, channel, val & 127, (val >> 7) & 127);
}

static void vsthost_midi_sysex(t_vsthost *x, t_symbol *s, int argc, t_atom *argv){
    if (!x->check_plugin()) return;

    std::string data;
    data.reserve(argc);
    for (int i = 0; i < argc; ++i){
        data.push_back((unsigned char)atom_getfloat(argv+i));
    }

    x->x_plugin->sendSysexEvent(VSTSysexEvent(std::move(data)));
}

// programs
static void vsthost_program_set(t_vsthost *x, t_floatarg _index){
    if (!x->check_plugin()) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumPrograms()){
        x->x_plugin->setProgram(index);
        x->x_editor->update();
	} else {
        pd_error(x, "%s: program number %d out of range!", classname(x), index);
	}
}

static void vsthost_program_get(t_vsthost *x){
    if (!x->check_plugin()) return;
	t_atom msg;
    SETFLOAT(&msg, x->x_plugin->getProgram());
    outlet_anything(x->x_messout, gensym("program"), 1, &msg);
}

static void vsthost_program_setname(t_vsthost *x, t_symbol* name){
    if (!x->check_plugin()) return;
    x->x_plugin->setProgramName(name->s_name);
}

static void vsthost_program_name(t_vsthost *x, t_symbol *s, int argc, t_atom *argv){
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

static void vsthost_program_count(t_vsthost *x){
    if (!x->check_plugin()) return;
	t_atom msg;
    SETFLOAT(&msg, x->x_plugin->getNumPrograms());
	outlet_anything(x->x_messout, gensym("program_count"), 1, &msg);
}

static void vsthost_program_list(t_vsthost *x){
    int n = x->x_plugin->getNumPrograms();
    t_atom msg[2];
    for (int i = 0; i < n; ++i){
        SETFLOAT(&msg[0], i);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getProgramNameIndexed(i).c_str()));
        outlet_anything(x->x_messout, gensym("program_name"), 2, msg);
    }
}

// read/write FX programs
static void vsthost_program_setdata(t_vsthost *x, t_symbol *s, int argc, t_atom *argv){
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

static void vsthost_program_data(t_vsthost *x){
    if (!x->check_plugin()) return;
    std::string buffer;
    x->x_plugin->writeProgramData(buffer);
    int n = buffer.size();
    std::vector<t_atom> atoms;
    atoms.resize(n);
    for (int i = 0; i < n; ++i){
            // first convert to range 0-255, then assign to t_float (not 100% portable...)
        SETFLOAT(&atoms[i], (unsigned char)buffer[i]);
    }
    outlet_anything(x->x_messout, gensym("program_data"), n, atoms.data());
}

static void vsthost_program_read(t_vsthost *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char dir[MAXPDSTRING], *name;
    int fd = canvas_open(x->x_editor->canvas(), s->s_name, "", dir, &name, MAXPDSTRING, 1);
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

static void vsthost_program_write(t_vsthost *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char path[MAXPDSTRING];
    canvas_makefilename(x->x_editor->canvas(), s->s_name, path, MAXPDSTRING);
    x->x_plugin->writeProgramFile(path);
}

// read/write FX banks
static void vsthost_bank_setdata(t_vsthost *x, t_symbol *s, int argc, t_atom *argv){
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

static void vsthost_bank_data(t_vsthost *x){
    if (!x->check_plugin()) return;
    std::string buffer;
    x->x_plugin->writeBankData(buffer);
    int n = buffer.size();
    std::vector<t_atom> atoms;
    atoms.resize(n);
    for (int i = 0; i < n; ++i){
            // first convert to range 0-255, then assign to t_float (not 100% portable...)
        SETFLOAT(&atoms[i], (unsigned char)buffer[i]);
    }
    outlet_anything(x->x_messout, gensym("bank_data"), n, atoms.data());
}

static void vsthost_bank_read(t_vsthost *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char dir[MAXPDSTRING], *name;
    int fd = canvas_open(x->x_editor->canvas(), s->s_name, "", dir, &name, MAXPDSTRING, 1);
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

static void vsthost_bank_write(t_vsthost *x, t_symbol *s){
    if (!x->check_plugin()) return;
    char path[MAXPDSTRING];
    canvas_makefilename(x->x_editor->canvas(), s->s_name, path, MAXPDSTRING);
    x->x_plugin->writeBankFile(path);
}

// plugin version
static void vsthost_version(t_vsthost *x){
    if (!x->check_plugin()) return;
    int version = x->x_plugin->getPluginVersion();
	t_atom msg;
	SETFLOAT(&msg, version);
	outlet_anything(x->x_messout, gensym("version"), 1, &msg);
}


/**** private ****/

// helper methods
bool t_vsthost::check_plugin(){
    if (x_plugin){
        return true;
    } else {
        pd_error(this, "%s: no plugin loaded!", classname(this));
        return false;
    }
}

void t_vsthost::check_precision(){
    if (x_plugin){
        if (!x_plugin->hasSinglePrecision() && !x_plugin->hasDoublePrecision()) {
            post("%s: '%s' doesn't support single or double precision, bypassing",
                classname(this), x_plugin->getPluginName().c_str());
        }
        if (x_dp && !x_plugin->hasDoublePrecision()){
            post("%s: '%s' doesn't support double precision, using single precision instead",
                 classname(this), x_plugin->getPluginName().c_str());
        }
        else if (!x_dp && !x_plugin->hasSinglePrecision()){ // very unlikely...
            post("%s: '%s' doesn't support single precision, using double precision instead",
                 classname(this), x_plugin->getPluginName().c_str());
        }
    }
}

void t_vsthost::update_buffer(){
        // the input/output buffers must be large enough to fit both
        // the number of Pd inlets/outlets and plugin inputs/outputs.
        // this routine is called in the "dsp" method and when a plugin is loaded.
    int blocksize = x_blocksize;
    int nin = x_nin;
    int nout = x_nout;
    int pin = 0;
    int pout = 0;
    if (x_plugin){
        pin = x_plugin->getNumInputs();
        pout = x_plugin->getNumOutputs();
    }
    int ninbuf = std::max(pin, nin);
    int noutbuf = std::max(pout, nout);
    int inbufsize = ninbuf * sizeof(double) * blocksize;
    int outbufsize = noutbuf * sizeof(double) * blocksize;
    x_inbuf = (char*)resizebytes(x_inbuf, x_inbufsize, inbufsize);
    x_outbuf = (char*)resizebytes(x_outbuf, x_outbufsize, outbufsize);
    x_inbufsize = inbufsize;
    x_outbufsize = outbufsize;
    x_inbufvec = (void**)resizebytes(x_inbufvec, x_ninbuf * sizeof(void *), ninbuf * sizeof(void *));
    x_outbufvec = (void**)resizebytes(x_outbufvec, x_noutbuf * sizeof(void *), noutbuf * sizeof(void *));
    x_ninbuf = ninbuf;
    x_noutbuf = noutbuf;
}

// constructor
// usage: vsthost~ [flags...] [file] inlets (default=2) outlets (default=2)
static void *vsthost_new(t_symbol *s, int argc, t_atom *argv){
    t_vsthost *x = (t_vsthost *)pd_new(vsthost_class);

    int generic = 0; // use generic Pd editor
    int dp = (PD_FLOATSIZE == 64); // default precision
    t_symbol *file = nullptr; // plugin to load (optional)

    while (argc && argv->a_type == A_SYMBOL){
        const char *flag = atom_getsymbol(argv)->s_name;
        if (*flag == '-'){
            switch (flag[1]){
            case 'g':
                generic = 1;
                break;
            case 'd':
                dp = 1;
                break;
            case 's':
                dp = 0;
                break;
            default:
                break;
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

        // VST plugin
    x->x_plugin = nullptr;
    x->x_bypass = 0;
    x->x_blocksize = 64;
    x->x_sr = 44100;
    x->x_dp = dp;

        // inputs (skip first):
    for (int i = 1; i < in; ++i){
		inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
	}
    x->x_nin = in;
    x->x_invec = (t_float**)getbytes(sizeof(t_float*) * in);
        // outlets:
	for (int i = 0; i < out; ++i){
		outlet_new(&x->x_obj, &s_signal);
	}
    x->x_nout = out;
    x->x_outvec = (t_float**)getbytes(sizeof(t_float*) * out);

        // editor
    x->x_editor = new t_vsteditor(*x, generic);

        // buffers
    x->x_inbufsize = 0;
    x->x_inbuf = nullptr;
    x->x_ninbuf = 0;
    x->x_inbufvec = nullptr;
    x->x_outbufsize = 0;
    x->x_outbuf = nullptr;
    x->x_noutbuf = 0;
    x->x_outbufvec = nullptr;

    x->x_f = 0;
    x->x_messout = outlet_new(&x->x_obj, 0);

    if (file){
        vsthost_open(x, file);
    }
    return (x);
}

// destructor
static void vsthost_free(t_vsthost *x){
    vsthost_close(x);
        // buffers
    freebytes(x->x_invec, x->x_nin * sizeof(t_float*));
    freebytes(x->x_outvec, x->x_nout * sizeof(t_float*));
    freebytes(x->x_inbuf, x->x_inbufsize);
    freebytes(x->x_outbuf, x->x_outbufsize);
    freebytes(x->x_inbufvec, x->x_ninbuf * sizeof(void*));
    freebytes(x->x_outbufvec, x->x_noutbuf * sizeof(void*));
        // editor
    delete x->x_editor;
}

// perform routine
static t_int *vsthost_perform(t_int *w){
    t_vsthost *x = (t_vsthost *)(w[1]);
    int n = (int)(w[2]);
    auto plugin = x->x_plugin;
    int nin = x->x_nin;
    t_float ** invec = x->x_invec;
    int nout = x->x_nout;
    t_float ** outvec = x->x_outvec;
    int ninbuf = x->x_ninbuf;
    void ** inbufvec = x->x_inbufvec;
    int noutbuf = x->x_noutbuf;
    void ** outbufvec = x->x_outbufvec;
    int out_offset = 0;
    bool dp = x->x_dp;
    bool bypass = plugin ? x->x_bypass : true;

    if(plugin && !bypass) {
            // check processing precision (single or double)
        if (!plugin->hasSinglePrecision() && !plugin->hasDoublePrecision()) {
            bypass = true;
        } else if (dp && !plugin->hasDoublePrecision()){
            dp = false;
        } else if (!dp && !plugin->hasSinglePrecision()){ // very unlikely...
            dp = true;
        }
    }

    if (!bypass){  // process audio
        int pin = plugin->getNumInputs();
        int pout = plugin->getNumOutputs();
        out_offset = pout;
            // process in double precision
        if (dp){
                // prepare input buffer
            for (int i = 0; i < ninbuf; ++i){
                double *buf = (double *)x->x_inbuf + i * n;
                inbufvec[i] = buf;
                if (i < nin){  // copy from Pd input
                    t_float *in = invec[i];
                    for (int j = 0; j < n; ++j){
                        buf[j] = in[j];
                    }
                } else {  // zero
                    for (int j = 0; j < n; ++j){
                        buf[j] = 0;
                    }
                }
            }
                // set output buffer pointers
            for (int i = 0; i < pout; ++i){
                outbufvec[i] = ((double *)x->x_outbuf + i * n);
            }
                // process
            plugin->processDouble((double **)inbufvec, (double **)outbufvec, n);
                // read from output buffer
            for (int i = 0; i < nout && i < pout; ++i){
                t_float *out = outvec[i];
                double *buf = (double *)outbufvec[i];
                for (int j = 0; j < n; ++j){
                    out[j] = buf[j];
                }
            }
        } else {  // single precision
                // prepare input buffer
            for (int i = 0; i < ninbuf; ++i){
                float *buf = (float *)x->x_inbuf + i * n;
                inbufvec[i] = buf;
                if (i < nin){  // copy from Pd input
                    t_float *in = invec[i];
                    for (int j = 0; j < n; ++j){
                        buf[j] = in[j];
                    }
                } else {  // zero
                    for (int j = 0; j < n; ++j){
                        buf[j] = 0;
                    }
                }
            }
                // set output buffer pointers
            for (int i = 0; i < pout; ++i){
                outbufvec[i] = ((float *)x->x_outbuf + i * n);
            }
                // process
            plugin->process((float **)inbufvec, (float **)outbufvec, n);
                // read from output buffer
            for (int i = 0; i < nout && i < pout; ++i){
                t_float *out = outvec[i];
                float *buf = (float *)outbufvec[i];
                for (int j = 0; j < n; ++j){
                    out[j] = buf[j];
                }
            }
        }
    } else {  // just pass it through
        t_float *buf = (t_float *)x->x_inbuf;
        t_float *bufptr;
        out_offset = nin;
            // copy input
        bufptr = buf;
        for (int i = 0; i < nin && i < nout; ++i){
            t_float *in = invec[i];
            for (int j = 0; j < n; ++j){
                *bufptr++ = in[j];
            }
        }
            // write output
        bufptr = buf;
        for (int i = 0; i < nin && i < nout; ++i){
            t_float *out = outvec[i];
            for (int j = 0; j < n; ++j){
                out[j] = *bufptr++;
            }
        }
    }
        // zero remaining outlets
    for (int i = out_offset; i < nout; ++i){
        auto vec = x->x_outvec[i];
        for (int j = 0; j < n; ++j){
            vec[j] = 0;
        }
    }

    return (w+3);
}

// dsp callback
static void vsthost_dsp(t_vsthost *x, t_signal **sp){
    int blocksize = sp[0]->s_n;
    t_float sr = sp[0]->s_sr;
    dsp_add(vsthost_perform, 2, x, blocksize);
    x->x_blocksize = blocksize;
    x->x_sr = sr;
    if (x->x_plugin){
        x->x_plugin->setBlockSize(blocksize);
        x->x_plugin->setSampleRate(sr);
    }
    int nin = x->x_nin;
    int nout = x->x_nout;
    for (int i = 0; i < nin; ++i){
        x->x_invec[i] = sp[i]->s_vec;
    }
    for (int i = 0; i < nout; ++i){
        x->x_outvec[i] = sp[nin + i]->s_vec;
    }
    x->update_buffer();
}

// setup function
extern "C" {

void vsthost_tilde_setup(void)
{
    vsthost_class = class_new(gensym("vsthost~"), (t_newmethod)vsthost_new, (t_method)vsthost_free,
        sizeof(t_vsthost), 0, A_GIMME, A_NULL);
    CLASS_MAINSIGNALIN(vsthost_class, t_vsthost, x_f);
    class_addmethod(vsthost_class, (t_method)vsthost_dsp, gensym("dsp"), A_CANT, A_NULL);
        // plugin
    class_addmethod(vsthost_class, (t_method)vsthost_open, gensym("open"), A_SYMBOL, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_close, gensym("close"), A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_bypass, gensym("bypass"), A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_vis, gensym("vis"), A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_click, gensym("click"), A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_precision, gensym("precision"), A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_version, gensym("version"), A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_info, gensym("info"), A_NULL);
        // parameters
    class_addmethod(vsthost_class, (t_method)vsthost_param_set, gensym("param_set"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_param_get, gensym("param_get"), A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_param_name, gensym("param_name"), A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_param_label, gensym("param_label"), A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_param_display, gensym("param_display"), A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_param_count, gensym("param_count"), A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_param_list, gensym("param_list"), A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_param_dump, gensym("param_dump"), A_NULL);
        // midi
    class_addmethod(vsthost_class, (t_method)vsthost_midi_raw, gensym("midi_raw"), A_GIMME, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_midi_note, gensym("midi_note"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_midi_noteoff, gensym("midi_noteoff"), A_FLOAT, A_FLOAT, A_DEFFLOAT, A_NULL); // third floatarg is optional!
    class_addmethod(vsthost_class, (t_method)vsthost_midi_cc, gensym("midi_cc"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_midi_bend, gensym("midi_bend"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_midi_program_change, gensym("midi_program_change"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_midi_aftertouch, gensym("midi_aftertouch"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_midi_channel_aftertouch, gensym("midi_channel_aftertouch"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_midi_sysex, gensym("midi_sysex"), A_GIMME, A_NULL);
        // programs
    class_addmethod(vsthost_class, (t_method)vsthost_program_set, gensym("program_set"), A_FLOAT, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_program_get, gensym("program_get"), A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_program_setname, gensym("program_setname"), A_SYMBOL, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_program_name, gensym("program_name"), A_GIMME, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_program_count, gensym("program_count"), A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_program_list, gensym("program_list"), A_NULL);
        // read/write fx programs
    class_addmethod(vsthost_class, (t_method)vsthost_program_setdata, gensym("program_setdata"), A_GIMME, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_program_data, gensym("program_data"), A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_program_read, gensym("program_read"), A_SYMBOL, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_program_write, gensym("program_write"), A_SYMBOL, A_NULL);
        // read/write fx banks
    class_addmethod(vsthost_class, (t_method)vsthost_bank_setdata, gensym("bank_setdata"), A_GIMME, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_bank_data, gensym("bank_data"), A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_bank_read, gensym("bank_read"), A_SYMBOL, A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_bank_write, gensym("bank_write"), A_SYMBOL, A_NULL);

    vstparam_setup();

    VSTWindowFactory::initialize();
}

} // extern "C"
