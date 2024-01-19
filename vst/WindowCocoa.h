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
- (BOOL)performKeyEquivalent:(NSEvent *)event;
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

    void open() override;
    void close() override;
    void setPos(int x, int y) override;
    void setSize(int w, int h) override;

    void resize(int w, int h) override;
    
    void doOpen();
    void onClose();
    void onResize(int w, int h);
    void updateEditor();
 private:
    void *getHandle();
    void updateFrame();
    bool canResize() const;

    CocoaEditorWindow * window_ = nullptr;
    IPlugin *plugin_;
    NSTimer *timer_;
    Rect rect_{ 100, 100, 0, 0 }; // empty rect!
    bool adjustSize_ = false;
    bool adjustPos_ = false;
    bool loading_ = false;

    static std::atomic<int> numWindows_;

    struct Command {
        Window *owner;
        int x;
        int y;
    };
};

} // Cocoa
} // vst
