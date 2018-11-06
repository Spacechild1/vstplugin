#pragma once

#include "VSTPluginInterface.h"

class VSTWindow {
public:
    virtual ~VSTWindow() {}

    virtual void* getHandle() = 0; // get system-specific handle to the window (to be passed to effEditOpen)

    virtual void setGeometry(int left, int top, int right, int bottom) = 0;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void minimize() = 0;
    virtual void restore() = 0; // un-minimize
    virtual void bringToTop() = 0;

    virtual bool isRunning() const = 0; // message loop still running?
protected:
    virtual void run() = 0; // start the message loop
};

namespace VSTWindowFactory {
    VSTWindow* create(IVSTPlugin& plugin);
}
