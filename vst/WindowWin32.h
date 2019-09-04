#pragma once

#include "Interface.h"

#include <windows.h>
#include <condition_variable>
#include <mutex>

namespace vst {
namespace Win32 {

enum Message {
    WM_CREATE_PLUGIN = WM_USER + 100,
    WM_DESTROY_PLUGIN,
    WM_OPEN_EDITOR,
    WM_CLOSE_EDITOR,
    WM_EDITOR_POS,
    WM_EDITOR_SIZE
};

namespace UIThread {

const int updateInterval = 30;

class EventLoop {
 public:
    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
    bool postMessage(UINT msg, void *data = nullptr); // non-blocking
    bool sendMessage(UINT msg, void *data = nullptr); // blocking
    HANDLE threadHandle() { return thread_; }
 private:
    struct PluginData {
        const PluginInfo* info;
        IPlugin::ptr plugin;
        Error err;
    };
    static DWORD WINAPI run(void *user);
    LRESULT WINAPI procedure(HWND hWnd, UINT Msg,
                        WPARAM wParam, LPARAM lParam);
    void notify();
    HANDLE thread_;
    DWORD threadID_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool ready_ = false;
};

} // UIThread

class Window : public IWindow {
 public:
    static LRESULT WINAPI procedure(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

    Window(IPlugin& plugin);
    ~Window();

    void* getHandle() override {
        return hwnd_;
    }

    void setTitle(const std::string& title) override;

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;
    void update();
    IPlugin* plugin() { return plugin_; }
 private:
    static const UINT_PTR timerID = 0x375067f6;
    static void CALLBACK updateEditor(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);
    void setGeometry(int left, int top, int right, int bottom);
    HWND hwnd_ = nullptr;
    IPlugin* plugin_ = nullptr;
};

} // Win32
} // vst
