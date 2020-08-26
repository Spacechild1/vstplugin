#import "WindowCocoa.h"
#include "Utility.h"

#if __has_feature(objc_arc)
#error This file must be compiled without ARC!
#endif

#include <iostream>


@implementation CocoaEditorWindow {}

- (void)setOwner:(vst::IWindow *)owner {
    owner_ = owner;
}

- (BOOL)windowShouldClose:(id)sender {
    LOG_DEBUG("window should close");
    static_cast<vst::Cocoa::Window *>(owner_)->onClose();
    return YES;
}

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize {
    LOG_DEBUG("window will resize");
    return frameSize;
}

- (void)windowDidResize:(NSNotification *)notification {
    // LATER verify size
    // get content size from frame size
    NSRect contentRect = [self contentRectForFrameRect:[self frame]];
    // resize editor
    static_cast<vst::Cocoa::Window *>(owner_)->plugin().resizeEditor(
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
    static_cast<vst::Cocoa::Window *>(owner_)->updateEditor();
}
}

@end

namespace vst {

namespace UIThread {

static std::atomic_bool gRunning;

void setup(){
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
}

void run() {
    // this doesn't work...
    // [NSApp run];
    // Kudos to https://www.cocoawithlove.com/2009/01/demystifying-nsapplication-by.html
    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];

    [NSApp finishLaunching];
    gRunning = true;

    while (gRunning) {
        [pool release];
        pool = [[NSAutoreleasePool alloc] init];
        NSEvent* event = [NSApp nextEventMatchingMask:NSAnyEventMask
                                            untilDate:[NSDate distantFuture]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event) {
            [NSApp sendEvent:event];
            [NSApp updateWindows];
        }
    }
    [pool release];
}

void quit() {
    // break from event loop instead of [NSApp terminate:nil]
    gRunning = false;
    // send dummy event to wake up event loop
    NSEvent* event = [NSEvent otherEventWithType:NSApplicationDefined
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:NO];
}

bool isCurrentThread(){
    return [NSThread isMainThread];
}

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

bool callSync(Callback cb, void *user){
    return Cocoa::EventLoop::instance().callSync(cb, user);
}

bool callAsync(Callback cb, void *user){
    return Cocoa::EventLoop::instance().callAsync(cb, user);
}

int32_t addPollFunction(PollFunction fn, void *context){
    return Cocoa::EventLoop::instance().addPollFunction(fn, context);
}

void removePollFunction(int32_t handle){
    return Cocoa::EventLoop::instance().removePollFunction(handle);
}

} // UIThread

namespace Cocoa {

EventLoop& EventLoop::instance(){
    static EventLoop e;
    return e;
}

EventLoop::EventLoop(){
    // we must access NSApp only once in the beginning (why?)
    haveNSApp_ = (NSApp != nullptr);
    if (haveNSApp_){
        // transform process into foreground application
        ProcessSerialNumber psn = {0, kCurrentProcess};
        TransformProcessType(&psn, kProcessTransformToForegroundApplication);
        LOG_DEBUG("init cocoa event loop");
    } else {
        LOG_WARNING("The host application doesn't have a UI thread (yet?), so I can't show the VST GUI editor.");
    }

    auto createTimer = [this](){
        timer_ = [NSTimer scheduledTimerWithTimeInterval:(updateInterval * 0.001)
                    target:nil
                    selector:@selector(poll)
                    userInfo:nil
                    repeats:YES];
    };

    if (UIThread::isCurrentThread()){
        createTimer();
    } else {
        auto queue = dispatch_get_main_queue();
        dispatch_async(queue, createTimer);
    }
}

EventLoop::~EventLoop(){}

UIThread::Handle EventLoop::addPollFunction(UIThread::PollFunction fn,
                                            void *context){
    std::lock_guard<std::mutex> lock(pollFunctionMutex_);
    auto handle = nextPollFunctionHandle_++;
    pollFunctions_.emplace(handle, [context, fn](){ fn(context); });
    return handle;
}

void EventLoop::removePollFunction(UIThread::Handle handle){
    std::lock_guard<std::mutex> lock(pollFunctionMutex_);
    pollFunctions_.erase(handle);
}

void EventLoop::poll(){
    std::lock_guard<std::mutex> lock(pollFunctionMutex_);
    for (auto& it : pollFunctions_){
        it.second();
    }
}

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
        window_ = window;
        [window setOwner:this];
        [[NSNotificationCenter defaultCenter] addObserver:window selector:@selector(windowDidResize:)
                name:NSWindowDidResizeNotification object:window];
        
        // set window title
        NSString *title = @(plugin_->info().name.c_str());
        [window setTitle:title];

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
        timer_ = nil;
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
