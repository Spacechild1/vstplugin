#import "WindowCocoa.h"
#include "Utility.h"

#if __has_feature(objc_arc)
#error This file must be compiled without ARC!
#endif

#include <iostream>


@implementation CocoaEditorWindow {}

@synthesize owner = _owner;

- (BOOL)windowShouldClose:(id)sender {
    LOG_DEBUG("window should close");
    vst::IWindow *owner = [self owner];
    static_cast<vst::Cocoa::Window *>(owner)->onClose();
    return YES;
}

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize {
    LOG_DEBUG("window will resize");
    return frameSize;
}

- (void)windowDidResize:(NSNotification *)notification {
    // LATER verify size
    vst::IWindow *owner = [self owner];
    // get content size from frame size
    NSRect contentRect = [self contentRectForFrameRect:[self frame]];
    // resize editor
    static_cast<vst::Cocoa::Window *>(owner)->plugin().resizeEditor(
        contentRect.size.width, contentRect.size.height);
    LOG_DEBUG("window did resize");
}

- (void)windowDidMiniaturize:(NSNotification *)notification {
    LOG_DEBUG("window miniaturized");
}
- (void)windowDidDeminiaturize:(NSNotification *)notification {
    LOG_DEBUG("window deminiaturized");
}
- (void)windowDidMove:(NSNotification *)notification {
    LOG_DEBUG("window did move");
}
- (void)updateEditor {
    vst::IWindow *owner = [self owner];
    static_cast<vst::Cocoa::Window *>(owner)->updateEditor();
}

@end

namespace vst {

namespace UIThread {

void setup(){
#if !HAVE_UI_THREAD
    if (isCurrentThread()){
        // create NSApplication in this thread (= main thread)
        // check if someone already created NSApp (just out of curiousity)
        if (NSApp != nullptr){
            LOG_WARNING("NSApp already initialized!");
            return;
        }
        // NSApp will automatically point to the NSApplication singleton
        [NSApplication sharedApplication];
        LOG_DEBUG("init cocoa event loop (polling)");
    } else {
        // we don't run on the main thread and expect the host app to create
        // the event loop (the EventLoop constructor will warn us otherwise).
    }
#endif
}

bool isCurrentThread(){
    return [NSThread isMainThread];
}

#if !HAVE_UI_THREAD
void poll(){
    // only on the main thread!
    if (isCurrentThread()){
        NSAutoreleasePool *pool =[[NSAutoreleasePool alloc] init];
        while (true) {
            NSEvent *event = [NSApp
                nextEventMatchingMask:NSAnyEventMask
                untilDate:[[NSDate alloc] init]
                inMode:NSDefaultRunLoopMode
                dequeue:YES];
            if (event){
                [NSApp sendEvent:event];
                [NSApp updateWindows];
                // LOG_DEBUG("got event: " << [event type]);
            } else {
                break;
            }
        }
        [pool release];
    }
}
#endif

bool callSync(Callback cb, void *user){
    return Cocoa::EventLoop::instance().callSync(cb, user);
}

bool callAsync(Callback cb, void *user){
    return Cocoa::EventLoop::instance().callAsync(cb, user);
}

} // UIThread

namespace Cocoa {

EventLoop& EventLoop::instance(){
    static EventLoop thread;
    return thread;
}

EventLoop::EventLoop(){
    // transform process into foreground application
    ProcessSerialNumber psn = {0, kCurrentProcess};
    TransformProcessType(&psn, kProcessTransformToForegroundApplication);
    // we must access NSApp only once in the beginning (why?)
    haveNSApp_ = (NSApp != nullptr);
#if HAVE_UI_THREAD
    if (haveNSApp_){
        LOG_DEBUG("init cocoa event loop");
    } else {
        LOG_WARNING("The host application doesn't have a UI thread (yet?), so I can't show the VST GUI editor.");
    }
#endif
}

EventLoop::~EventLoop(){}

bool EventLoop::callSync(UIThread::Callback cb, void *user){
    if (haveNSApp_){
        if (UIThread::isCurrentThread()){
            cb(user); // we're on the main thread
        } else {
            auto queue = dispatch_get_main_queue();
            dispatch_sync_f(queue, user, cb);
        }
        return true;
    } else {
        return false;
    }
}

bool EventLoop::callAsync(UIThread::Callback cb, void *user){
    if (haveNSApp_){
        if (UIThread::isCurrentThread()){
            cb(user); // we're on the main thread
        } else {
            auto queue = dispatch_get_main_queue();
            dispatch_async_f(queue, user, cb);
        }
        return true;
    } else {
        return false;
    }
}

Window::Window(IPlugin& plugin)
    : plugin_(&plugin) {
    origin_ = NSMakePoint(100, 100); // default position (bottom left)
}

// the destructor must be called on the main thread!
Window::~Window(){
    if (window_){
        plugin_->closeEditor();
        [timer_ invalidate];
        [window_ close];
    }
    LOG_DEBUG("destroyed Window");
}

void Window::open(){
    LOG_DEBUG("show window");
    UIThread::callAsync([](void *x){
        static_cast<Window *>(x)->doOpen();
    }, this);
}

// to be called on the main thread
void Window::doOpen(){
    if (window_){
        [window_ makeKeyAndOrderFront:nil];
        return;
    }

    NSRect frame = NSMakeRect(0, 0, 200, 200);
    NSUInteger style = NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask;
    if (canResize_ || plugin_->canResize()) {
        style |= NSResizableWindowMask;
        canResize_ = true;
        LOG_DEBUG("can resize");
    }
    CocoaEditorWindow *window = [[CocoaEditorWindow alloc] initWithContentRect:frame
                styleMask:style
                backing:NSBackingStoreBuffered
                defer:NO];
    if (window){
        [window setOwner:this];
        window_ = window;
        [[NSNotificationCenter defaultCenter] addObserver:window selector:@selector(windowDidResize:) name:NSWindowDidResizeNotification object:window];
        
        setTitle(plugin_->info().name);
        int left = 100, top = 100, right = 400, bottom = 400;
        bool gotEditorRect = plugin_->getEditorRect(left, top, right, bottom);
        if (gotEditorRect){
            setFrame(origin_.x, origin_.y, right - left, bottom - top);
        }
        
        plugin_->openEditor(getHandle());

        // HACK for plugins which don't report the window size without the editor being opened
        if (!gotEditorRect){
            plugin_->getEditorRect(left, top, right, bottom);
            setFrame(origin_.x, origin_.y, right - left, bottom - top);
        }

        timer_ = [NSTimer scheduledTimerWithTimeInterval:(EventLoop::updateInterval * 0.001)
                    target:window
                    selector:@selector(updateEditor)
                    userInfo:nil
                    repeats:YES];

        [window_ makeKeyAndOrderFront:nil];
        LOG_DEBUG("created Window");
        LOG_DEBUG("window size: " << (right - left) << " * " << (bottom - top));
    }
}

void Window::close(){
    LOG_DEBUG("hide window");
    UIThread::callAsync([](void *x){
        [static_cast<Window *>(x)->window_ performClose:nil];
    }, this);
}

// to be called on the main thread
void Window::onClose(){
    if (window_){
        [[NSNotificationCenter defaultCenter] removeObserver:window_ name:NSWindowDidResizeNotification object:window_];
        [timer_ invalidate];
        plugin_->closeEditor();
        origin_ = [window_ frame].origin;
        window_ = nullptr;
    }
}

void Window::updateEditor(){
    plugin_->updateEditor();
}

void * Window::getHandle(){
    return window_ ? [window_ contentView] : nullptr;
}

void Window::setTitle(const std::string& title){
    if (window_){
        NSString *name = @(title.c_str());
        [window_ setTitle:name];
    }
}

void Window::setFrame(int x, int y, int w, int h){
    if (window_){
        if (adjustY_){
            y  -= window_.frame.size.height;
            adjustY_ = false;
        }
        NSRect content = NSMakeRect(x, y, w, h);
        NSRect frame = [window_  frameRectForContentRect:content];
        [window_ setFrame:frame display:YES];
    }
}

void Window::setPos(int x, int y){
    // on Cocoa, the y-axis is inverted (goes up).
    NSScreen *screen = nullptr;
    if (window_){
        [window_ setFrameOrigin:NSMakePoint(x, y)];
        // First move the window to the given x coordinate,
        // then obtain the screen height.
        screen = [window_ screen];
    } else {
        screen = [NSScreen mainScreen];
    }
    origin_.x = x;
    // now correct the y coordinate
    origin_.y = screen.frame.size.height - y;
    if (window_){
        origin_.y -= window_.frame.size.height;
        [window_ setFrameOrigin:origin_];
    } else {
        // no window height, adjust later
        adjustY_ = true;
    }
}

void Window::setSize(int w, int h){
    if (window_){
        NSPoint origin = window_.frame.origin;
        setFrame(origin.x, origin.y, w, h);
    }
}

} // Cocoa

IWindow::ptr IWindow::create(IPlugin &plugin){
    return std::make_unique<Cocoa::Window>(plugin);
}

} // vst
