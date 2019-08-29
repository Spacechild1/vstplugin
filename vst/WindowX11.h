#pragma once

#include "Interface.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <list>

namespace vst {
namespace X11 {

namespace UIThread {

const int updateInterval = 30;

class EventLoop {
 public:
    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
    bool postClientEvent(Atom atom, long data = 0);
    std::thread::id threadID(){ return thread_.get_id(); }
 private:
    void run();
    void updatePlugins();
    Display *display_ = nullptr;
    ::Window root_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
    const PluginInfo* info_ = nullptr;
    IPlugin::ptr plugin_;
    Error err_;
    bool ready_ = false;
    std::list<IPlugin *> pluginList_;
    std::thread timerThread_;
    std::atomic<bool> timerThreadRunning_{true};
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
};

} // X11
} // vst
