#pragma once

#include "Interface.h"
#include "PluginManager.h"
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
    int x_blocksize = 64;
    t_float x_sr = 44100;
    std::vector<t_sample *> x_siginlets;
    std::vector<t_sample *> x_sigoutlets;
    std::vector<t_sample *> x_sigauxinlets;
    std::vector<t_sample *> x_sigauxoutlets;
    std::vector<char> x_inbuf;
    std::vector<char> x_auxinbuf;
    std::vector<char> x_outbuf;
    std::vector<char> x_auxoutbuf;
    // VST plugin
    IPlugin::ptr x_plugin;
    t_symbol *x_path = nullptr;
    bool x_uithread = false;
    bool x_keep = false;
    Bypass x_bypass = Bypass::Off;
    ProcessPrecision x_precision; // single/double precision
    double x_lastdsptime = 0;
    std::shared_ptr<t_vsteditor> x_editor;
    // search
    struct t_search_data {
        std::vector<t_symbol *> s_plugins;
        std::atomic_bool s_running { false };
    };
    using t_search_data_ptr = std::shared_ptr<t_search_data>;
    t_clock *x_search_clock;
    t_search_data_ptr x_search_data;
    // methods
    bool open_plugin(t_symbol *s, bool editor);
    void set_param(int index, float param, bool automated);
    void set_param(int index, const char *s, bool automated);
    bool check_plugin();
    void setup_plugin();
    int get_sample_offset();
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
class t_vsteditor : public IPluginListener {
 public:
    t_vsteditor(t_vstplugin &owner, bool gui);
    ~t_vsteditor();
    // setup the generic Pd editor
    void setup();
    // update the parameter displays
    void update();
    // notify generic GUI for parameter changes
    void param_changed(int index, float value, bool automated = false);
    // flush parameter, MIDI and sysex queues
    void flush_queues();
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
 private:
    // plugin callbacks
    void parameterAutomated(int index, float value) override;
    void midiEvent(const MidiEvent& event) override;
    void sysexEvent(const SysexEvent& event) override;
    // helper functions
    void send_mess(t_symbol *sel, int argc = 0, t_atom *argv = 0){
        if (e_canvas) pd_typedmess((t_pd *)e_canvas, sel, argc, argv);
    }
    template<typename... T>
    void send_vmess(t_symbol *sel, const char *fmt, T... args){
        if (e_canvas) pd_vmess((t_pd *)e_canvas, sel, (char *)fmt, args...);
    }
    // notify Pd (e.g. for MIDI event or GUI automation)
    template<typename T, typename U>
    void post_event(T& queue, U&& event);
    static void tick(t_vsteditor *x);
    // data
    t_vstplugin *e_owner;
    t_canvas *e_canvas = nullptr;
    std::vector<t_vstparam> e_params;
    // outgoing messages:
    t_clock *e_clock;
#if HAVE_UI_THREAD
    std::mutex e_mutex;
    std::thread::id e_mainthread;
    std::atomic_bool e_needclock {false};
#endif
    std::vector<std::pair<int, float>> e_automated;
    std::vector<MidiEvent> e_midi;
    std::vector<SysexEvent> e_sysex;
    bool e_tick = false;
    int width_ = 0;
    int height_ = 0;
};
