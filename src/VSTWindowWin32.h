#include "VSTPluginInterface.h"

#include <windows.h>
#include <process.h>
#include <atomic>
#include <thread>

class VSTWindowWin32 : public IVSTWindow {
public:
    VSTWindowWin32(IVSTPlugin& plugin);

    ~VSTWindowWin32();

    void* getHandle() override {
        return hwnd_;
    }

    void setGeometry(int left, int top, int right, int bottom) override;

    void show() override;
    void hide() override;
    void minimize() override;
    void restore() override;
    void bringToTop() override;

    bool isRunning() const override {
        return bRunning_.load();
    }
protected:
    void run() override;
private:
    HWND hwnd_{nullptr};
    std::thread thread_;
    std::atomic<bool> bRunning_{false};

    void threadFunction(IVSTPlugin *plugin);
};
