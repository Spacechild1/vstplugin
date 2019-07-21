#pragma once

#include "Interface.h"

#include <windows.h>
#include <condition_variable>
#include <mutex>

namespace vst {
namespace Win32 {

const UINT WM_CREATE_PLUGIN = WM_USER + 100;
const UINT WM_DESTROY_PLUGIN = WM_USER + 101;

class UIThread {
 public:
    static UIThread& instance();

    UIThread();
    ~UIThread();

    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
    bool postMessage(UINT msg, WPARAM wparam = 0, LPARAM lparam = 0);
 private:
    static DWORD WINAPI run(void *user);
    LRESULT WINAPI proc(HWND hWnd, UINT Msg,
                        WPARAM wParam, LPARAM lParam);
    HANDLE thread_;
    DWORD threadID_;
    std::mutex mutex_;
    std::condition_variable cond_;
    const PluginInfo* info_ = nullptr;
    IPlugin::ptr plugin_;
    Error err_;
    bool ready_ = false;
};

class Window : public IWindow {
 public:
    Window(IPlugin& plugin);
    ~Window();

    void* getHandle() override {
        return hwnd_;
    }

    void setTitle(const std::string& title) override;
    void setGeometry(int left, int top, int right, int bottom) override;

    void show() override;
    void hide() override;
    void minimize() override;
    void restore() override;
    void bringToTop() override;
    void update() override;
 private:
    HWND hwnd_ = nullptr;
    IPlugin* plugin_ = nullptr;
};

} // Win32
} // vst
