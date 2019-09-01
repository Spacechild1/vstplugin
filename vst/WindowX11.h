#pragma once

#include "Interface.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>

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
    bool postClientEvent(::Window window, Atom atom, long data1 = 0, long data2 = 0);
    bool sendClientEvent(::Window, Atom atom, long data1 = 0, long data2 = 0);
    std::thread::id threadID(){ return thread_.get_id(); }
 private:
    struct PluginData {
        const PluginInfo* info;
        IPlugin::ptr plugin;
        Error err;
    };
    void run();
    void notify();
    void updatePlugins();
    Display *display_ = nullptr;
    ::Window root_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool ready_ = false;
    PluginData data_; // we can't send 64bit pointers with X11 client messages...
    std::unordered_map<::Window, IPlugin *> pluginMap_;
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

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;
    void doOpen();
    void doClose();
 private:
    Display *display_;
    IPlugin *plugin_;
    ::Window window_ = 0;
    bool mapped_ = false;
    int x_ = 100;
    int y_ = 100;
};

} // X11
} // vst
