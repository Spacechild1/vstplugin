#include "Interface.h"

#import <Cocoa/Cocoa.h>

@interface CocoaEditorWindow : NSWindow {
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
namespace Cocoa {
    
namespace UIThread {

class EventLoop {
 public:
    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
#if VSTTHREADS
    bool postMessage();
#endif
 private:
    IPlugin::ptr doCreate(const PluginInfo& info);
    void doDestroy(IPlugin::ptr plugin);
#if VSTTHREADS
    IPlugin::ptr plugin_;
    Error err_;
#endif
};

} // UIThread

class Window : public IWindow {
 public:
    Window(IPlugin& plugin);
    ~Window();

    void* getHandle() override;

    void setTitle(const std::string& title) override;
    void setGeometry(int left, int top, int right, int bottom) override;

    void show() override;
    void hide() override;
    void minimize() override;
    void restore() override;
    void bringToTop() override;
 private:
    CocoaEditorWindow * window_ = nullptr;
    IPlugin * plugin_;
};

} // Cocoa
} // vst
