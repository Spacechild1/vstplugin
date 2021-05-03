#pragma once

#include "Interface.h"
#include "Sync.h"

# ifndef NOMINMAX
#  define NOMINMAX
# endif
#include <windows.h>
#include <mutex>
#include <functional>
#include <unordered_map>

namespace vst {
namespace Win32 {

enum Message {
    WM_CALL = WM_APP + 2867,
    WM_SYNC
};

class EventLoop {
 public:
    static const int updateInterval = 30;

    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    bool sync();
    bool callAsync(UIThread::Callback cb, void *user); // blocking
    bool callSync(UIThread::Callback cb, void *user);

    UIThread::Handle addPollFunction(UIThread::PollFunction fn, void *context);
    void removePollFunction(UIThread::Handle handle);

    bool checkThread();
 private:
    bool postMessage(UINT msg, void *data1 = nullptr, void *data2 = nullptr); // non-blocking

    static DWORD WINAPI run(void *user);
    LRESULT WINAPI procedure(HWND hWnd, UINT Msg,
                        WPARAM wParam, LPARAM lParam);
    void notify();
    HANDLE thread_;
    DWORD threadID_;
    std::mutex mutex_;
    SyncCondition event_;

    UIThread::Handle nextPollFunctionHandle_ = 0;
    std::unordered_map<UIThread::Handle, std::function<void()>> pollFunctions_;
    std::mutex pollFunctionMutex_;
};

class Window : public IWindow {
 public:
    static LRESULT WINAPI procedure(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

    Window(IPlugin& plugin);
    ~Window();

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;

    void resize(int w, int h) override;
    void update() override;
 private:
    void doOpen();
    void doClose();
    void saveCurrentPosition();
    void updateFrame();
    void onSizing(RECT& newRect);
    void onSize(int w, int h);
    bool canResize();

    static const UINT_PTR timerID = 0x375067f6;
    static void CALLBACK updateEditor(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);
    HWND hwnd_ = nullptr;
    IPlugin* plugin_ = nullptr;
    Rect rect_{ 100, 100, 0, 0 }; // empty rect!
    bool adjustSize_ = false;
    bool canResize_ = false;
    bool didQueryResize_ = false;

    struct Command {
        Window *owner;
        int x;
        int y;
    };
};

} // Win32
} // vst
