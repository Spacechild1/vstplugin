#pragma once

#include "Interface.h"
#include "Sync.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace vst {
namespace X11 {

class Window;

class EventLoop {
 public:
    static const int updateInterval = 30;

    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    bool callSync(UIThread::Callback cb, void *user);
    bool callAsync(UIThread::Callback cb, void *user);

    bool postClientEvent(::Window window, Atom atom,
                         const char *data = nullptr, size_t size = 0);
    bool sendClientEvent(::Window, Atom atom,
                         const char *data = nullptr, size_t size = 0);

    bool checkThread();
    Display *getDisplay() { return display_; }

    void registerWindow(Window& w);
    void unregisterWindow(Window& w);
 private:
    void run();
    void notify();
    void updatePlugins();
    Display *display_ = nullptr;
    ::Window root_;
    std::thread thread_;
    std::mutex mutex_;
    Event event_;
    std::unordered_map<::Window, IPlugin *> pluginMap_;
    std::thread timerThread_;
    std::atomic<bool> timerThreadRunning_{true};
};

class Window : public IWindow {
 public:
    Window(Display &display, IPlugin& plugin);
    ~Window();

    void* getHandle() override {
        return (void*)window_;
    }

    IPlugin* getPlugin() { return plugin_; }

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;
    void doOpen();
    void doClose();
    void doUpdate();
    void onConfigure(int x, int y, int width, int height);
 private:
    Display *display_;
    IPlugin *plugin_;
    ::Window window_ = 0;
    bool mapped_ = false;
    int x_;
    int y_;
    int width_;
    int height_;
};

} // X11
} // vst
