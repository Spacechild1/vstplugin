#pragma once

#include "Interface.h"

#include <windows.h>

namespace vst {

namespace WindowFactory {
    void initializeWin32();
    IWindow::ptr createWin32(IPlugin::ptr plugin);
}

class WindowWin32 : public IWindow {
 public:
    WindowWin32(IPlugin::ptr plugin);
    ~WindowWin32();

    void* getHandle() override {
        return hwnd_;
    }
    void run() override;
    void quit() override;

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
    IPlugin::ptr plugin_;
};

} // vst
