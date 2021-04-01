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

#if USE_VST3
    using EventHandlerCallback = void (*)(int fd, void *obj);
    void registerEventHandler(int fd, EventHandlerCallback cb, void *obj);
    void unregisterEventHandler(void *obj);

    using TimerCallback = void (*)(void *obj);
    void registerTimer(int64_t ms, TimerCallback cb, void *obj);
    void unregisterTimer(void *obj);
#endif
 private:
    bool postClientEvent(::Window window, Atom atom,
                         const char *data = nullptr, size_t size = 0);
    void run();
    void updatePlugins();
    Window* findWindow(::Window handle);
    void pollEvents();

    Display *display_ = nullptr;
    ::Window root_;
    std::thread thread_;
    std::mutex mutex_;
    SyncCondition event_;
    std::vector<Window *> windows_;
#if USE_VST3
    struct EventHandler {
        void *obj;
        EventHandlerCallback cb;
    };
    std::unordered_map<int, EventHandler> eventHandlers_;
    std::mutex eventMutex_;
    struct Timer {
        void *obj = nullptr;
        TimerCallback cb;
        int64_t interval;
        int64_t elapsed;
    };
    std::vector<Timer> timerList_;
#endif
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

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;

    void resize(int w, int h) override;

    void onClose();
    void onConfigure(int x, int y, int width, int height);
    void onUpdate();

    void *getHandle() { return (void *)window_; }
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
