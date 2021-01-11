#include "Interface.h"

#import <Cocoa/Cocoa.h>

// TODO: this probably should be a window *delegate*, so we don't need
// the NoficationCenter hack.
@interface CocoaEditorWindow : NSWindow {
    vst::IWindow *owner_;
}

- (void)setOwner:(vst::IWindow *)owner;
- (BOOL)windowShouldClose:(id)sender;
- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize;
- (void)windowDidResize:(NSNotification *)notification;
- (void)windowDidMiniaturize:(NSNotification *)notification;
- (void)windowDidDeminiaturize:(NSNotification *)notification;
- (void)windowDidMove:(NSNotification *)notification;
- (void)updateEditor;

@end

// EventLoopProxy

namespace vst { namespace Cocoa {
class EventLoop;
}}

@interface EventLoopProxy : NSObject {
    vst::Cocoa::EventLoop *owner_;
}

- (id)initWithOwner:(vst::Cocoa::EventLoop*)owner;
- (void)poll;

@end

namespace vst {
namespace Cocoa {

class EventLoop {
 public:
    static const int updateInterval = 30;

    static EventLoop& instance();

    EventLoop();
    ~EventLoop();

    bool callSync(UIThread::Callback cb, void *user);

    bool callAsync(UIThread::Callback cb, void *user);

    UIThread::Handle addPollFunction(UIThread::PollFunction fn, void *context);

    void removePollFunction(UIThread::Handle handle);

    void poll();

    bool available() const {
        return haveNSApp_;
    }
 private:
    bool haveNSApp_ = false;
    EventLoopProxy *proxy_;
    NSTimer *timer_;
    UIThread::Handle nextPollFunctionHandle_ = 0;
    std::unordered_map<UIThread::Handle, std::function<void()>> pollFunctions_;
    std::mutex pollFunctionMutex_;
};

class Window : public IWindow {
 public:
    Window(IPlugin& plugin);
    ~Window();

    void* getHandle() override;

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

    static std::atomic<int> numWindows_;
};

} // Cocoa
} // vst
