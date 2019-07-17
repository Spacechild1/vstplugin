#include "VSTPluginInterface.h"

#import <Cocoa/Cocoa.h>

@interface VSTEditorWindow : NSWindow {
    vst::IVSTPlugin *_plugin;
}

@property (nonatomic, readwrite) vst::IVSTPlugin *plugin;

- (BOOL)windowShouldClose:(id)sender;
/*
- (void)windowDidMiniaturize:(id)sender;
- (void)windowDidDeminiaturize:(id)sender;
*/
@end

namespace vst {

namespace VSTWindowFactory {
    void initializeCocoa();
    void pollCocoa();
    IVSTWindow::ptr createCocoa(IVSTPlugin::ptr plugin);
}

class VSTWindowCocoa : public IVSTWindow {
 public:
    VSTWindowCocoa(IVSTPlugin::ptr plugin);
    ~VSTWindowCocoa();

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
    IVSTPlugin::ptr plugin_;
};

} // vst
