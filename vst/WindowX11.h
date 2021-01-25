#pragma once

#include "Interface.h"
#include "Sync.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
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

    bool sync();
    bool callSync(UIThread::Callback cb, void *user);
    bool callAsync(UIThread::Callback cb, void *user);

    bool checkThread();
    Display *getDisplay() { return display_; }
    ::Window getRoot() { return root_; }

    UIThread::Handle addPollFunction(UIThread::PollFunction fn, void *context);
    void removePollFunction(UIThread::Handle handle);

    void registerWindow(Window* w);
    void unregisterWindow(Window* w);
 private:
    bool postClientEvent(::Window window, Atom atom,
                         const char *data = nullptr, size_t size = 0);
    void run();
    void updatePlugins();
    Window* findWindow(::Window handle);

    Display *display_ = nullptr;
    ::Window root_;
    std::thread thread_;
    std::mutex mutex_;
    SyncCondition event_;
    std::vector<Window *> windows_;
    std::thread timerThread_;
    std::atomic<bool> timerThreadRunning_{true};
    UIThread::Handle nextPollFunctionHandle_ = 0;
    std::unordered_map<UIThread::Handle, std::function<void()>> pollFunctions_;
    std::mutex pollFunctionMutex_;
};

class Window : public IWindow {
 public:
    Window(Display &display, IPlugin& plugin);
    ~Window();

    void* getHandle() override {
        return (void*)window_;
    }

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;
    void onClose();
    void onConfigure(int x, int y, int width, int height);
    void onUpdate();
 private:
    Display *display_;
    IPlugin *plugin_;
    ::Window window_ = 0;

    Rect rect_{ 100, 100, 0, 0 }; // empty rect!
    bool canResize_ = false;

    // helper methods
    void doOpen();
    void doClose();
    void setFixedSize(int w, int h);
    void savePosition();

    struct Command {
        Window *owner;
        int x;
        int y;
    };
};

} // X11
} // vst
