#include "VSTPluginInterface.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

class VSTWindowX11 : public IVSTWindow {
 public:
    VSTWindowX11(IVSTPlugin* plugin);
    ~VSTWindowX11();

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
    IVSTPlugin *plugin_ = nullptr;
    Window window_ = 0;
    Atom wmProtocols_;
    Atom wmDelete_;
    Atom wmQuit_; // custom quit message
};
