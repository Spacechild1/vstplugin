#pragma once

#include "VSTPluginInterface.h"
#include "Utility.h"

#include "m_pd.h"

#include <memory>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#ifndef VSTTHREADS
#define VSTTHREADS 1
#endif

#if VSTTHREADS
#include <atomic>
#include <thread>
#include <future>
#endif

class t_vsteditor;

// vstplugin~ object (plain C struct without constructors/destructors!)
struct t_vstplugin {
    t_object x_obj;
    t_sample x_f;
    t_outlet *x_messout;
        // VST plugin
    IVSTPlugin* x_plugin;
    int x_bypass;
    int x_blocksize;
    int x_sr;
    int x_gui;
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
        // internal methods
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
class t_vsteditor : IVSTPluginListener {
 public:
    t_vsteditor(t_vstplugin &owner);
    ~t_vsteditor();
        // open the plugin (and launch GUI thread if needed)
    IVSTPlugin* open_plugin(const char* path, bool gui);
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
#if VSTTHREADS
        // open plugin in a new thread
    void thread_function(std::promise<IVSTPlugin *> promise, const char *path);
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

