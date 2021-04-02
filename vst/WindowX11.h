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
    static const int sleepGrain = 5;
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

    using EventHandlerCallback = void (*)(int fd, void *obj);
    void registerEventHandler(int fd, EventHandlerCallback cb, void *obj);
    void unregisterEventHandler(void *obj);

    using TimerCallback = void (*)(void *obj);
    void registerTimer(int64_t ms, TimerCallback cb, void *obj);
    void unregisterTimer(void *obj);
 private:
    void pushCommand(UIThread::Callback cb, void *obj);
    void doRegisterTimer(int64_t ms, TimerCallback cb, void *obj);
    void doUnregisterTimer(void *obj);
    void run();
    void pollEvents();
    void pollFds();
    void notify();
    Window* findWindow(::Window handle);

    Display *display_ = nullptr;
    ::Window root_;
    std::thread thread_;
    int eventfd_ = -1;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::mutex syncMutex_;
    SyncCondition event_;
    std::vector<Window *> windows_;

    struct Command {
        UIThread::Callback cb;
        void *obj;
    };
    std::vector<Command> commands_;

    struct EventHandler {
        void *obj;
        EventHandlerCallback cb;
    };
    std::unordered_map<int, EventHandler> eventHandlers_;

    struct Timer {
        void *obj = nullptr;
        TimerCallback cb;
        int64_t interval;
        double elapsed;
    };
    std::vector<Timer> timerList_;

    UIThread::Handle nextPollFunctionHandle_ = 0;
    std::unordered_map<UIThread::Handle, void *> pollFunctions_;
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
