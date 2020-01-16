#include "Interface.h"

#import <Cocoa/Cocoa.h>

// TODO: this probably should be a window *delegate*, so we don't need
// the NoficationCenter hack.
@interface CocoaEditorWindow : NSWindow {
    vst::IWindow *_owner;
}

@property (nonatomic, readwrite) vst::IWindow *owner;

- (BOOL)windowShouldClose:(id)sender;
- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize;
- (void)windowDidResize:(NSNotification *)notification;
- (void)windowDidMiniaturize:(NSNotification *)notification;
- (void)windowDidDeminiaturize:(NSNotification *)notification;
- (void)windowDidMove:(NSNotification *)notification;
- (void)updateEditor;

@end

namespace vst {
namespace Cocoa {
    
namespace UIThread {

const int updateInterval = 30;

class EventLoop {
 public:
    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    IPlugin::ptr create(const PluginInfo& info);
    void destroy(IPlugin::ptr plugin);
#if HAVE_UI_THREAD
    bool postMessage();
 private:
    bool haveNSApp_ = false;
#endif
};

} // UIThread

class Window : public IWindow {
 public:
    Window(IPlugin& plugin);
    ~Window();

    void* getHandle() override;

    void setTitle(const std::string& title) override;

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;
    
    void doOpen();
    void onClose();
    void updateEditor();
    IPlugin& plugin() { return *plugin_; }
 private:
    void setFrame(int x, int y, int w, int h);
    CocoaEditorWindow * window_ = nullptr;
    IPlugin *plugin_;
    NSTimer *timer_;
    NSPoint origin_;
    bool adjustY_ = false;
    // HACK: at least one plugin only reports "canResize" correctly
    // the very first time and then always returns, so we cache
    // "true" results.
    bool canResize_ = false;
};

} // Cocoa
} // vst
