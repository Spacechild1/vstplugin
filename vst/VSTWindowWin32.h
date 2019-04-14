#pragma once

#include "VSTPluginInterface.h"

#include <windows.h>

namespace vst {

namespace VSTWindowFactory {
    void initializeWin32();
    IVSTWindow * createWin32(IVSTPlugin &plugin);
}

class VSTWindowWin32 : public IVSTWindow {
 public:
    VSTWindowWin32(IVSTPlugin &plugin);
    ~VSTWindowWin32();

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
    IVSTPlugin *plugin_ = nullptr;
};

} // vst
