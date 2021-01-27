#import "WindowCocoa.h"
#include "Utility.h"

#if __has_feature(objc_arc)
#error This file must be compiled without ARC!
#endif

#include <iostream>
#include <dispatch/dispatch.h>

// CocoaEditorWindow

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

@end

// EventLoopProxy

@implementation EventLoopProxy
- (id)initWithOwner:(vst::Cocoa::EventLoop*)owner {
    self = [super init];
    if (!self) return nil;

    owner_ = owner;
    return self;
}

- (void)poll {
    owner_->poll();
}
@end

namespace vst {

namespace UIThread {

static std::atomic_bool gRunning;

void setup(){
    Cocoa::EventLoop::instance();
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

bool isCurrentThread() {
    return [NSThread isMainThread];
}

bool available() {
    return Cocoa::EventLoop::instance().available();
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

bool sync(){
    return callSync([](void *){}, nullptr);
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

/*////////////////////// EventLoop ////////////////////*/

EventLoop& EventLoop::instance(){
    static EventLoop e;
    return e;
}

EventLoop::EventLoop(){
    // NOTE: somehow we must access NSApp only once in the beginning to check
    // for its existence (why?), that's why we cache the result in 'haveNSApp_'.
    if (UIThread::isCurrentThread()){
        // create NSApplication in this thread (= main thread)
        // check if someone already created NSApp (just out of curiousity)
        if (NSApp != nullptr){
            LOG_WARNING("NSApp already initialized!");
        } else {
            // NSApp will automatically point to the NSApplication singleton
            [NSApplication sharedApplication];
            LOG_DEBUG("init cocoa event loop (polling)");
        }
        haveNSApp_ = true;
    } else {
        // we don't run on the main thread and expect the host app
        // to create NSApp and run the event loop.
        haveNSApp_ = (NSApp != nullptr);
        if (!haveNSApp_){
            LOG_WARNING("The host application doesn't have a UI thread (yet?), so I can't show the VST GUI editor.");
            return; // done
        }
        LOG_DEBUG("init cocoa event loop");
    }

    proxy_ = [[EventLoopProxy alloc] initWithOwner:this];

    auto createTimer = [this](){
        timer_ = [NSTimer scheduledTimerWithTimeInterval:(updateInterval * 0.001)
                    target:proxy_
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

EventLoop::~EventLoop(){
    if (haveNSApp_){
        [timer_ invalidate];
        timer_ = nil;
        [proxy_ release];
    }
}

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
    // LOG_DEBUG("poll functions");
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
        LOG_DEBUG("callSync() failed - no NSApp");
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
        LOG_DEBUG("callAsync() failed - no NSApp");
        return false;
    }
}

/*///////////////// Window ///////////////////////*/

std::atomic<int> Window::numWindows_{0};

Window::Window(IPlugin& plugin)
    : plugin_(&plugin) {
    canResize_ = plugin_->canResize();
}

Window::~Window(){
    if (window_){
    #if 1
        // will implicitly call onClose()!
        [window_ performClose:nil];
    #else
        // cache window before it is set to NULL in onClose()
        auto window = window_;
        onClose();
        [window close];
    #endif
    }
    LOG_DEBUG("Cocoa: destroyed Window");
}

void Window::open(){
    UIThread::callAsync([](void *x){
        static_cast<Window *>(x)->doOpen();
    }, this);
}

// to be called on the main thread
void Window::doOpen(){
    if (window_){
        // just bring to top
        [NSApp activateIgnoringOtherApps:YES];
        [window_ makeKeyAndOrderFront:nil];
        LOG_DEBUG("Cocoa: restore")
        return;
    }

    NSRect frame = NSMakeRect(0, 0, 200, 200);
    NSUInteger style = NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask;
    if (canResize_) {
        style |= NSResizableWindowMask;
        LOG_DEBUG("Cocoa: can resize");
    }
    window_ = [[CocoaEditorWindow alloc] initWithContentRect:frame
                styleMask:style
                backing:NSBackingStoreBuffered
                defer:NO];
    if (window_){
        [window_ setOwner:this];
        [[NSNotificationCenter defaultCenter] addObserver:window selector:@selector(windowDidResize:)
                name:NSWindowDidResizeNotification object:window_];
        
        // set window title
        NSString *title = @(plugin_->info().name.c_str());
        [window_ setTitle:title];
        LOG_DEBUG("Cocoa: created Window");

        // set window coordinates
        bool didOpen = false;
        if (rect_.valid()){
            LOG_DEBUG("Cocoa: use cached editor size");
            setFrame(rect_, false);
        } else {
            // get window dimensions from plugin
            Rect r;
            if (!plugin_->getEditorRect(r)){
                // HACK for plugins which don't report the window size
                // without the editor being opened
                LOG_DEBUG("Cocoa: couldn't get editor rect!");
                plugin_->openEditor(getHandle());
                plugin_->getEditorRect(r);
                didOpen = true;
            }
            LOG_DEBUG("Cocoa: editor size " << r.w << " * " << r.h);
            // bash x and y values
            r.x = rect_.x;
            r.y = rect_.y;
            // adjust and set window dimensions
            setFrame(r, true);
        }

        if (!didOpen){
            plugin->openEditor(getHandle());
        }

        timer_ = [NSTimer scheduledTimerWithTimeInterval:(EventLoop::updateInterval * 0.001)
                    target:window_
                    selector:@selector(updateEditor)
                    userInfo:nil
                    repeats:YES];

        if (numWindows_.fetch_add(1) == 0){
            // first Window: transform process into foreground application.
            // This is necessariy so we can table cycle the Window(s)
            // and access them from the dock.
            // NOTE: we have to do this *before* bringing the window to the top
            ProcessSerialNumber psn = {0, kCurrentProcess};
            TransformProcessType(&psn, kProcessTransformToForegroundApplication);
        }

        // bring to top
        [NSApp activateIgnoringOtherApps:YES];
        [window_ makeKeyAndOrderFront:nil];

        LOG_DEBUG("Cocoa: opened Window");
    }
}

void Window::close(){
    EventLoop::instance().callAsync([](void *x){
        auto window = static_cast<Window *>(x)->window_;
        // will implicitly call onClose()!
        [window performClose:nil];
    }, this);
}

// to be called on the main thread
void Window::onClose(){
    if (window_){
        [[NSNotificationCenter defaultCenter] removeObserver:window_ name:NSWindowDidResizeNotification object:window_];

        [timer_ invalidate];
        timer_ = nil;

        plugin_->closeEditor();

        auto r = [window_ frame]; // adjusted
        // cache position and size
        rect_.x = r.origin.x;
        rect_.y = r.origin.y;
        rect_.w = r.size.width;
        rect_.h = r.size.height;
        needAdjustSize_ = false;
        needAdjustPos_ = false;

        window_ = nullptr;

        if (numWindows_.fetch_sub(1) == 1){
            // last Window: transform back into background application
            ProcessSerialNumber psn = {0, kCurrentProcess};
            TransformProcessType(&psn, kProcessTransformToUIElementApplication);
        }

        LOG_DEBUG("Cocoa: closed Window");
    }
}

void Window::updateEditor(){
    plugin_->updateEditor();
}

void * Window::getHandle(){
    return window_ ? [window_ contentView] : nullptr;
}

void Window::setFrame(Rect r, bool adjustSize, bool adjustPos){
    if (window_){
        // first adjust size, because we need it to adjust pos!
        if (adjustSize || needAdjustSize_){
            adjustSize(r);
            needAdjustSize_ = false;
        }
        if (adjustPos || needAdjustPos_){
            adjustPos(r);
            needAdjustPos_ = false;
        }
        NSRect frame = NSMakeRect(r.x, r.y, r.w, r.h);
        [window_ setFrame:frame display:YES];
    } else {
        if (adjustSize){
            needAdjustSize_ = true;;
        }
        if (adjustPos){
            needAdjustPos_ = true;
        }
    }
    // cache (adjusted)  rect
    rect_ = r;
}

void Window::adjustRect(Rect& r){
    NSRect content = NSMakeRect(r.x, r.y, r.w, r.h);
    NSRect frame = [window_  frameRectForContentRect:content];
    r.w = frame.size.width;
    r.h = frame.size.height;
}

void Window::adjustPos(Rect& r){
    // First move the window to the given x coordinate
    [window_ setFrameOrigin:NSMakePoint(r.x, r.y)];
    // then obtain the screen height.
    auto height = window_.screen.frame.size.height;
    // flip y coordinate
    r.y = height - r.y;
    // adjust for window height
    r.y -= window_.frame.size.height;
}

void Window::setPos(int x, int y){
    EventLoop::instance().callAsync([](void *user){
        auto cmd = static_cast<Command *>(user);
        auto owner = cmd->owner;
        Rect r = owner->rect_;
        r.x = cmd->x;
        r.y = cmd->y;
        owner->setFrame(r, false, true); // only adjust pos!
        delete cmd;
    }, new Command { this, x, y });
}

void Window::setSize(int w, int h){
    LOG_DEBUG("setSize: " << w << ", " << h);
    EventLoop::instance().callAsync([](void *user){
        auto cmd = static_cast<Command *>(user);
        auto owner = cmd->owner;
        // only if we can resize (or don't know)
        if (!owner->hwnd_ || canResize_){
            Rect r = owner->rect_;
            r.w = cmd->x;
            r.h = cmd->y;
            owner->setFrame(r, true);
        }
        delete cmd;
    }, new Command { this, w, h });
}

void Window::resize(int w, int h){
    LOG_DEBUG("resized by plugin: " << w << ", " << h);
    Rect r { rect_.x, rect_.y, w, h };
    setFrame(r, true);
}

} // Cocoa

IWindow::ptr IWindow::create(IPlugin &plugin){
    return std::make_unique<Cocoa::Window>(plugin);
}

} // vst
