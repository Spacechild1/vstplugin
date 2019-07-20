#pragma once

#include "VSTPluginInterface.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace vst {

namespace WindowFactory {
    void initializeX11();
    IWindow::ptr createX11(IPlugin::ptr plugin);
}

class WindowX11 : public IWindow {
 public:
    WindowX11(IPlugin::ptr plugin);
    ~WindowX11();

    void* getHandle() override {
        return (void*)window_;
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
 private:
    Display *display_ = nullptr;
    IPlugin::ptr plugin_;
    Window window_ = 0;
    Atom wmProtocols_;
    Atom wmDelete_;
    Atom wmQuit_; // custom quit message
};

} // vst
