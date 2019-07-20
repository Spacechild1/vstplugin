#include "Interface.h"

#import <Cocoa/Cocoa.h>

@interface VSTEditorWindow : NSWindow {
    vst::IPlugin *_plugin;
}

@property (nonatomic, readwrite) vst::IPlugin *plugin;

- (BOOL)windowShouldClose:(id)sender;
/*
- (void)windowDidMiniaturize:(id)sender;
- (void)windowDidDeminiaturize:(id)sender;
*/
@end

namespace vst {

namespace WindowFactory {
    void initializeCocoa();
    void pollCocoa();
    IWindow::ptr createCocoa(IPlugin::ptr plugin);
}

class WindowCocoa : public IWindow {
 public:
    WindowCocoa(IPlugin::ptr plugin);
    ~WindowCocoa();

    void* getHandle() override;
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
    VSTEditorWindow * window_ = nullptr;
    IPlugin::ptr plugin_;
};

} // vst
