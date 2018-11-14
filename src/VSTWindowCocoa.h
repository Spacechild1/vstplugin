#include "VSTPluginInterface.h"

#import <Cocoa/Cocoa.h>

class VSTWindowCocoa : public IVSTWindow {
 public:
    VSTWindowCocoa();
    ~VSTWindowCocoa();

    void* getHandle() override;
    void run() override;

    void setTitle(const std::string& title) override;
    void setGeometry(int left, int top, int right, int bottom) override;

    void show() override;
    void hide() override;
    void minimize() override;
    void restore() override;
    void bringToTop() override;
 private:
    NSWindow * window_{nullptr};
    NSApplication * app_{nullptr};
};
