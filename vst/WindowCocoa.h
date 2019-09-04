#include "Interface.h"

#import <Cocoa/Cocoa.h>

@interface CocoaEditorWindow : NSWindow {
    vst::IWindow *_owner;
}

@property (nonatomic, readwrite) vst::IWindow *owner;

- (BOOL)windowShouldClose:(id)sender;
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
 private:
    void setGeometry(int left, int top, int right, int bottom);
    CocoaEditorWindow * window_ = nullptr;
    IPlugin * plugin_;
    NSPoint origin_;
    NSTimer *timer_;
};

} // Cocoa
} // vst
