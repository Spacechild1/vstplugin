#pragma once

#include "EventLoop.h"
#include "Interface.h"
#include "Log.h"
#include "Sync.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cassert>
#include <thread>
#include <mutex>
#include <atomic>
#include <list>
#include <unordered_map>

namespace vst {
namespace X11 {

class Window;

class EventLoop : public BaseEventLoop {
public:
    // TODO: is pollGrain really necessary?
    static constexpr int pollGrain = 10;

    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    bool available() const {
        return display_ != nullptr;
    }

    void run();
    void quit();
    bool sync();
    bool callSync(UIThread::Callback cb, void *user);
    bool callAsync(UIThread::Callback cb, void *user);

    Display *getDisplay() { return display_; }
    ::Window getRoot() { return root_; }

    void registerWindow(Window* w);
    void unregisterWindow(Window* w);

    using EventHandlerCallback = void (*)(int fd, void *obj);
    void registerEventHandler(int fd, EventHandlerCallback cb, void *obj);
    void unregisterEventHandler(void *obj);

    using TimerCallback = void (*)(void *obj);
    void registerTimer(int64_t ms, TimerCallback cb, void *obj);
    void unregisterTimer(void *obj);
private:
    void startPolling() override;
    void stopPolling() override;

    void initUIThread();
    void pushCommand(UIThread::Callback cb, void *obj);
    void doRegisterTimer(int64_t ms, TimerCallback cb, void *obj);
    void doUnregisterTimer(void *obj);
    int updateTimers();
    void pollFileDescriptors(int timeout);
    void pollX11Events();
    void handleCommands();
    void notify();
    Window* findWindow(::Window handle);

    Display *display_ = nullptr;
    ::Window root_ = 0;
    std::thread thread_;
    int eventfd_ = -1;
    std::atomic<bool> running_{false};
    std::mutex syncMutex_;
    SyncCondition event_;
    std::vector<Window *> windows_;

    struct Command {
        UIThread::Callback cb;
        void *obj;
    };
    std::vector<Command> commands_;
    std::mutex commandMutex_;

    struct EventHandler {
        void *obj;
        EventHandlerCallback cb;
    };
    std::unordered_map<int, EventHandler> eventHandlers_;

    using Clock = std::chrono::high_resolution_clock;
    using Milliseconds = std::chrono::milliseconds;
    using TimePoint = std::chrono::time_point<Clock, Milliseconds>;

    struct Timer {
        struct Compare {
            bool operator()(const Timer& a, const Timer& b) const {
                if (a.deadline > b.deadline) {
                    return true;
                } else if (a.deadline < b.deadline) {
                    return false;
                } else {
                    return a.sequence > b.sequence;
                }
            }
        };

        TimerCallback cb = nullptr;
        void *obj = nullptr;
        int64_t interval;
        uint64_t sequence; // keep insertion order
        TimePoint deadline;
    };
    std::vector<Timer> timerQueue_;
    uint64_t timerSequence_ = 0;
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
    bool canResize() const;

    Display *display_;
    IPlugin *plugin_;
    ::Window window_ = 0;

    Rect rect_{ 100, 100, 0, 0 }; // empty rect!

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
