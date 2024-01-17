#pragma once

#include "m_pd.h"

#include "Interface.h"
#include "PluginDictionary.h"
#include "Lockfree.h"
#include "Log.h"
#include "Bus.h"
#include "FileUtils.h"
#include "MiscUtils.h"
#include "Sync.h"
#include "CpuArch.h"

using namespace vst;

#include <memory>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <deque>
#include <unordered_map>
#include <type_traits>
#include <thread>
#include <mutex>
#include <fcntl.h>

// only try to poll event loop for macOS Pd standalone version
#if defined(__APPLE__) && !defined(PDINSTANCE)
#define POLL_EVENT_LOOP 1
#else
#define POLL_EVENT_LOOP 0
#endif

#undef pd_class
#define pd_class(x) (*(t_pd *)(x))
#define classname(x) (class_getname(pd_class(x)))

#if PD_MINOR_VERSION >= 54
# define PD_HAVE_MULTICHANNEL
#else
# pragma message("building without multi-channel support; requires Pd 0.54+")
# define CLASS_MULTICHANNEL 0
#endif

using t_signal_setmultiout = void (*)(t_signal **, int);

enum PdLogLevel {
    PdCritical = 0,
    PdError,
    PdNormal,
    PdDebug,
    PdVerbose
};

class t_vsteditor;
class t_vstplugin;

// base class for async commands
template<typename T>
struct t_command_data {
    static void free(T *x){ delete x; }
    using t_fun = void (*)(T *);

    t_vstplugin *owner = nullptr;
};

struct t_search_data : t_command_data<t_search_data> {
    std::vector<std::string> paths;
    std::vector<std::string> exclude;
    std::vector<PluginDesc::const_ptr> plugins;
#if WARN_VST3_PARAMETERS
    std::vector<PluginDesc::const_ptr> warn_plugins;
#endif
    std::string cachefiledir;
    float timeout;
    bool parallel;
    bool update;
    std::atomic_bool cancel {false};
};

// vstplugin~ object (everything public and no virtual methods!)
class t_vstplugin {
public:
    static constexpr const char *glob_recv_name = "__vstplugin~__"; // receive name shared by all instances

    t_vstplugin(int argc, t_atom *argv);
    ~t_vstplugin();
    // Pd
    t_object x_obj;
    t_sample x_f = 0;
    t_outlet *x_messout; // message outlet
    t_canvas *x_canvas; // parent canvas
    int x_blocksize = 0;
    t_float x_sr = 0;
    // signals
    struct t_signalbus {
        t_signalbus() = default;
        t_signalbus(int n) {
            if (n > 0) {
                resize(n);
            }
        }
        void resize(int n) {
            b_signals = std::make_unique<t_sample *[]>(n);
            b_buffers = std::make_unique<void *[]>(n);
            b_n = n;
        }

        std::unique_ptr<t_sample *[]> b_signals;
        std::unique_ptr<void *[]> b_buffers;
        int b_n = 0;
    };
    std::vector<t_signalbus> x_inlets;
    std::vector<t_signalbus> x_outlets;
    std::vector<Bus> x_inputs;
    std::vector<Bus> x_outputs;
    int x_input_channels = 0;
    int x_output_channels = 0;
    std::vector<char> x_inbuffer;
    std::vector<char> x_outbuffer;
    // VST plugin
    IPlugin::ptr x_plugin;
    std::unique_ptr<t_vsteditor> x_editor;
    Mutex x_mutex;
    bool x_multi = false;
    bool x_outchannels_changed = false;
    bool x_process = false;
    bool x_async = false;
    bool x_uithread = false;
    bool x_threaded = false;
    bool x_keep = false;
    bool x_suspended = false;
    Bypass x_bypass = Bypass::Off;
    ProcessPrecision x_wantprecision; // single/double precision
    ProcessPrecision x_realprecision;
    ProcessMode x_mode = ProcessMode::Realtime;
    RunMode x_runmode = RunMode::Auto;
    double x_lastdsptime = 0;
#ifdef PDINSTANCE
    t_pdinstance *x_pdinstance = nullptr; // keep track of the instance we belong to
#endif
    t_symbol *x_key = nullptr;
    t_symbol *x_path = nullptr;
    t_symbol *x_preset = nullptr;
    // search
    t_search_data * x_search_data = nullptr;
    // helper methods
    void set_param(int index, float param, bool automated);
    void set_param(int index, const char *s, bool automated);

    bool check_plugin();

    void setup_plugin(IPlugin& plugin);

    void update_buffers();

    int get_sample_offset();

    std::string resolve_plugin_path(const char *s);

    bool deferred() const {
        // NOTE: don't use x_runmode because we also
        // need to consider bit-bridging!
        return x_threaded || x_plugin->isBridged();
    }
};

// VST parameter responder (for Pd GUI)
class t_vstparam {
public:
    t_vstparam(t_vstplugin *x, int index);
    ~t_vstparam();

    t_vstparam(t_vstparam&&) noexcept;
    t_vstparam& operator=(t_vstparam&&) noexcept;

    void set(t_floatarg f);

    t_pd p_pd = nullptr; // !
    t_vstplugin *p_owner;
    t_symbol *p_slider;
    t_symbol *p_display_rcv;
    t_symbol *p_display_snd;
    int p_index;
private:
    void move(t_vstparam& other);
    void bind();
    void unbind();
};

// VST editor
class t_vsteditor : public IPluginListener {
public:
    enum {
        LatencyChange = -1
    };

    t_vsteditor(t_vstplugin& owner, bool gui);
    ~t_vsteditor();
    // setup the generic Pd editor
    void setup();
    // update the parameter displays
    void update(bool allow_defer);
    // notify generic GUI for parameter changes
    void param_changed(int index, float value, bool automated = false);
    // notify generic GUI for parameter changes
    void param_changed_deferred(int index, bool automated = false);
    // flush parameter, MIDI and sysex queues
    void flush_queues();
    // safely defer to UI thread
    template<bool async, typename T>
    void defer_safe(const T& fn, bool uithread);
    // show/hide window
    void vis(bool v);
    bool pd_gui() const {
        return e_canvas && !vst_gui();
    }
    bool vst_gui() const {
        return window() != nullptr;
    }
    IWindow * window() const {
        return e_owner->x_plugin ? e_owner->x_plugin->getWindow() : nullptr;
    }
    void set_pos(int x, int y);
    void set_size(int w, int h);
    // plugin callbacks
    void parameterAutomated(int index, float value) override;
    void latencyChanged(int nsamples) override;
    void updateDisplay() override;
    void pluginCrashed() override;
    void midiEvent(const MidiEvent& event) override;
    void sysexEvent(const SysexEvent& event) override;
private:
    // helper functions
    void send_mess(t_symbol *sel, int argc = 0, t_atom *argv = 0){
        if (e_canvas) pd_typedmess((t_pd *)e_canvas, sel, argc, argv);
    }
    template<typename... T>
    void send_vmess(t_symbol *sel, const char *fmt, T... args){
        if (e_canvas) pd_vmess((t_pd *)e_canvas, sel, (char *)fmt, args...);
    }
    // notify Pd (e.g. for MIDI event or GUI automation)
    struct t_event {
        enum t_type {
            Unknown,
            Parameter,
            Midi,
            Sysex,
            Latency,
            Crash,
            Display
        };
        t_event() : type(Unknown) {}
        t_event(t_type _type) : type(_type){}
        // data
        t_type type;
        union {
            int latency;
            struct {
                int index;
                float value;
            }  param;
            MidiEvent midi;
            SysexEvent sysex;
        };
    };
    void post_event(const t_event& event);
    static void tick(t_vsteditor *x);
    // data
    t_vstplugin *e_owner;
    t_canvas *e_canvas = nullptr;
    std::vector<t_vstparam> e_params;
    int width_ = 0;
    int height_ = 0;
    // outgoing messages:
    t_clock *e_clock;
    std::thread::id e_mainthread;
    std::atomic_bool e_needclock {false};
    std::atomic_bool e_locked {false};
    bool e_tick = false;
    UnboundedMPSCQueue<t_event> e_events;
    // for deferred parameter updates
    // The first half tells if/which parameters need to be updated;
    // the second half tells if the parameter has been automated.
    // For threaded plugins, the whole bitset is then duplicated
    // to allow double buffering.
    static constexpr size_t param_num_bits = 64;
    std::vector<std::bitset<param_num_bits>> e_param_bitset;
    int e_param_bitset_size = 0;
};

class t_workqueue {
public:
    template<typename T>
    using t_fun = void (*)(T *);

    static void init();
    static void release();
    static t_workqueue* get();

    t_workqueue();
    ~t_workqueue();

    template<typename T, typename Fn1, typename Fn2>
    void push(void *owner, T *data, Fn1 workfn, Fn2 cb){
        dopush(owner, data, t_fun<void>(t_fun<T>(workfn)),
               t_fun<void>(t_fun<T>(cb)), t_fun<void>(T::free));
    }
    void cancel(void *owner);
    void log(PdLogLevel level, std::string msg);
    void poll();

#ifdef PDINSTANCE
    static constexpr const char *bindname = "vstplugin__workqueue";

    t_pd w_pd; // object header
    // make everything public to be 100% sure that the compiler
    // can not reorder members (which is technically allowed
    // for classes with different access specifiers).
#else
private:
#endif
    struct t_item {
        void *owner;
        void *data;
        t_fun<void> workfn;
        t_fun<void> cb;
        t_fun<void> cleanup;
    };
    void dopush(void *owner, void *data, t_fun<void> workfn,
                t_fun<void> cb, t_fun<void> cleanup);
    // queues from RT to NRT
    UnboundedMPSCQueue<t_item> w_nrt_queue;
    // queue from NRT to RT
    UnboundedMPSCQueue<t_item> w_rt_queue;
    // worker thread
    std::thread w_thread;
    std::mutex w_mutex; // for cancellation
    Event w_event;
    std::atomic<bool> w_running{false};
    // logging
    struct t_logmsg {
        PdLogLevel level;
        std::string msg;
    };
    UnboundedMPSCQueue<t_logmsg> w_log_queue;
    // polling
    t_clock *w_clock = nullptr;
    static void clockmethod(t_workqueue *w);
#ifdef PDINSTANCE
    t_pdinstance *w_instance = nullptr;
    int w_refcount = 1;
#endif
};
