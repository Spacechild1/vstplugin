#pragma once

#include "EventLoop.h"
#include "Interface.h"
#include "Sync.h"

# ifndef NOMINMAX
#  define NOMINMAX
# endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <mutex>
#include <thread>
#include <functional>
#include <unordered_map>

#define VST_EDITOR_CLASS_NAME L"VST Plugin Editor Class"
#define VST_ROOT_CLASS_NAME L"VST Plugin Root Class"

// NB: can't synchronize threads in a global/static
// object constructor in a Windows DLL!
#ifndef JOIN_UI_THREAD
#define JOIN_UI_THREAD 0
#endif

namespace vst {
namespace Win32 {

enum Message {
    WM_CALL = WM_APP + 2867,
    WM_SYNC
};

class EventLoop : public BaseEventLoop {
public:
    static const UINT_PTR pollTimerID = 1;

    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    void run();
    void quit();
    bool sync();
    bool callAsync(UIThread::Callback cb, void *user);
    bool callSync(UIThread::Callback cb, void *user);
private:
    static LRESULT WINAPI procedure(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
    static HICON getIcon();

    bool postMessage(UINT msg, void *data1 = nullptr, void *data2 = nullptr); // non-blocking
    void handleTimer(UINT_PTR id);
    void initUIThread();
    void startPolling() override;
    void stopPolling() override;

    std::thread thread_;
    HWND hwnd_ = NULL;
    Mutex mutex_;
    SyncCondition event_;
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
    bool canResize() const;

    static const UINT_PTR timerID = 0x375067f6;
    static void CALLBACK updateEditor(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);
    HWND hwnd_ = nullptr;
    IPlugin* plugin_ = nullptr;
    Rect rect_{ 100, 100, 0, 0 }; // empty rect!
    bool adjustSize_ = false;

    struct Command {
        Window *owner;
        int x;
        int y;
    };
};

} // Win32
} // vst
