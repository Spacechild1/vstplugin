#pragma once

#include <string>
#include <memory>

class VSTWindow {
public:
    virtual ~VSTWindow() {}

    virtual void* getHandle() = 0; // get system-specific handle to the window (to be passed to effEditOpen)

    virtual void setGeometry(int left, int top, int right, int bottom) = 0;

    virtual void show() = 0;
    virtual void restore() = 0; // un-minimize
    virtual void top() = 0; // bring window to top

    virtual void run() = 0; // run the message queue (does not return until window is closed)
};

namespace VSTWindowFactory {
    VSTWindow* create(const std::string&name);
}
