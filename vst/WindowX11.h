#pragma once

#include "Interface.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace vst {
namespace X11 {

namespace UIThread {

class EventLoop {
 public:
    static Atom wmProtocols;
    static Atom wmDelete;
    static Atom wmQuit;
    static Atom wmCreatePlugin;
    static Atom wmDestroyPlugin;
    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
    bool postClientEvent(Atom atom);
 private:
    void run();
    Display *display_ = nullptr;
    ::Window root_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
    const PluginInfo* info_ = nullptr;
    IPlugin::ptr plugin_;
    Error err_;
    bool ready_ = false;
};

} // UIThread

class Window : public IWindow {
 public:
    Window(Display &display, IPlugin& plugin);
    ~Window();

    void* getHandle() override {
        return (void*)window_;
    }

    void setTitle(const std::string& title) override;
    void setGeometry(int left, int top, int right, int bottom) override;

    void show() override;
    void hide() override;
    void minimize() override;
    void restore() override;
    void bringToTop() override;
 private:
    Display *display_;
    IPlugin *plugin_;
    ::Window window_ = 0;
    Atom wmProtocols_;
    Atom wmDelete_;
};

} // X11
} // vst
