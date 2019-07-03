#pragma once

#include "VSTPluginInterface.h"
#include "VSTPluginManager.h"
#include "Utility.h"

using namespace vst;

#include "m_pd.h"

#include <memory>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <type_traits>
#include <thread>
#include <mutex>

#ifndef VSTTHREADS
#define VSTTHREADS 1
#endif

#if VSTTHREADS
#include <future>
#endif

enum PdLogLevel {
    PD_FATAL = -3,
    PD_ERROR,
    PD_NORMAL,
    PD_DEBUG,
    PD_ALL
};

class t_vsteditor;

// vstplugin~ object (no virtual methods!)
class t_vstplugin {
 public:
    t_vstplugin(int argc, t_atom *argv);
    ~t_vstplugin();
        // Pd
    t_object x_obj;
    t_sample x_f = 0;
    t_outlet *x_messout; // message outlet
    t_canvas *x_canvas; // parent canvas
    int x_blocksize = 0; // see vstplugin_dsp
    t_float x_sr = 44100;
    std::vector<t_sample *> x_siginlets;
    std::vector<t_sample *> x_sigoutlets;
        // VST plugin
    std::shared_ptr<IVSTPlugin> x_plugin;
    t_symbol *x_path = nullptr;
    bool x_keep = false;
    bool x_bypass = false;
    bool x_dp; // single/double precision
    std::unique_ptr<t_vsteditor> x_editor;
        // contiguous input/outputs buffer
    std::vector<char> x_inbuf;
    std::vector<char> x_outbuf;
        // array of input/output pointers
    std::vector<void *> x_invec;
    std::vector<void *> x_outvec;
        // thread for async operations (e.g. search)
    std::thread x_thread;
    t_clock *x_clock;
    std::vector<t_symbol *> x_plugins;
        // methods
    void set_param(int index, float param, bool automated);
    void set_param(int index, const char *s, bool automated);
    bool check_plugin();
    void update_buffer();
    void update_precision();
};

// VST parameter responder (for Pd GUI)
class t_vstparam {
 public:
    t_vstparam(t_vstplugin *x, int index);
    ~t_vstparam();
    void set(t_floatarg f);

    t_pd p_pd;
    t_vstplugin *p_owner;
    t_symbol *p_slider;
    t_symbol *p_display_rcv;
    t_symbol *p_display_snd;
    int p_index;
};

// VST editor
class t_vsteditor : public IVSTPluginListener {
 public:
    t_vsteditor(t_vstplugin &owner, bool gui);
    ~t_vsteditor();
        // open the plugin (and launch GUI thread if needed)
    std::shared_ptr<IVSTPlugin> open_plugin(const VSTPluginDesc& desc, bool editor);
        // close the plugin (and terminate GUI thread if needed)
    void close_plugin();
        // setup the generic Pd editor
    void setup();
        // update the parameter displays
    void update();
        // notify generic GUI for parameter changes
    void param_changed(int index, float value, bool automated = false);
        // show/hide window
    void vis(bool v);
    bool pd_gui() const {
        return e_canvas && !e_window;
    }
    bool vst_gui() const {
        return e_window != nullptr;
    }
 private:
        // plugin callbacks
    void parameterAutomated(int index, float value) override;
    void midiEvent(const VSTMidiEvent& event) override;
    void sysexEvent(const VSTSysexEvent& event) override;
        // helper functions
    void send_mess(t_symbol *sel, int argc = 0, t_atom *argv = 0){
        if (e_canvas) pd_typedmess((t_pd *)e_canvas, sel, argc, argv);
    }
    template<typename... T>
    void send_vmess(t_symbol *sel, const char *fmt, T... args){
        if (e_canvas) pd_vmess((t_pd *)e_canvas, sel, (char *)fmt, args...);
    }
#if VSTTHREADS
    using VSTPluginPromise = std::promise<std::shared_ptr<IVSTPlugin>>;
        // open plugin in a new thread
    void thread_function(VSTPluginPromise promise, const VSTPluginDesc *desc);
#endif
        // notify Pd (e.g. for MIDI event or GUI automation)
    template<typename T, typename U>
    void post_event(T& queue, U&& event);
    static void tick(t_vsteditor *x);
        // data
    t_vstplugin *e_owner;
#if VSTTHREADS
    std::thread e_thread;
    std::thread::id e_mainthread;
#endif
    std::unique_ptr<IVSTWindow> e_window;
    t_canvas *e_canvas = nullptr;
    std::vector<t_vstparam> e_params;
        // outgoing messages:
    t_clock *e_clock;
#if VSTTHREADS
    std::mutex e_mutex;
#endif
    std::vector<std::pair<int, float>> e_automated;
    std::vector<VSTMidiEvent> e_midi;
    std::vector<VSTSysexEvent> e_sysex;
};
