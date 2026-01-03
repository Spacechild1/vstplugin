#import "WindowCocoa.h"

#include "PluginDesc.h"
#include "Log.h"

#if __has_feature(objc_arc)
#error This file must be compiled without ARC!
#endif

#include <atomic>
#include <iostream>
#include <dispatch/dispatch.h>

// CocoaEditorWindow

@implementation CocoaEditorWindow {}

- (void)setOwner:(vst::IWindow *)owner {
    owner_ = owner;
}

- (BOOL)windowShouldClose:(id)sender {
    LOG_DEBUG("Cocoa: window should close");
    static_cast<vst::Cocoa::Window *>(owner_)->onClose();
    return YES;
}

- (void)windowDidMove:(NSNotification *)notification {
    // get position from *frame* rect
    auto rect = [self frame];
    auto pos = rect.origin;
    // get the screen height
    auto screenHeight = [self screen].frame.size.height;
    // flip y coordinate
    pos.y = screenHeight - (pos.y + rect.size.height);
    // notify window
    static_cast<vst::Cocoa::Window *>(owner_)->onMove(pos.x, pos.y);
}

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize {
    LOG_DEBUG("Cocoa: window will resize");
    return frameSize;
}

- (void)windowDidResize:(NSNotification *)notification {
    // LATER verify size
    // get size from *content* rect
    auto size = [self contentRectForFrameRect:[self frame]].size;
    // notify window
    static_cast<vst::Cocoa::Window *>(owner_)->onResize(size.width, size.height);
}

- (void)windowDidMiniaturize:(NSNotification *)notification {
    LOG_DEBUG("Cocoa: window miniaturized");
}

- (void)windowDidDeminiaturize:(NSNotification *)notification {
    LOG_DEBUG("Cocoa: window deminiaturized");
}

- (void)updateEditor {
    static_cast<vst::Cocoa::Window *>(owner_)->updateEditor();
}

- (BOOL)performKeyEquivalent:(NSEvent *)event {
    if (event.type == NSKeyDown){
        if (event.modifierFlags & NSCommandKeyMask){
            auto chars = event.charactersIgnoringModifiers.UTF8String;
            if (chars[0] == 'w'){
                LOG_DEBUG("Cocoa: Cmd+W");
                [self performClose:nil];
                return TRUE;
            }
        }
    }
    return FALSE;
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
    owner_->doPoll();
}
@end

namespace vst {

namespace UIThread {

static std::atomic<bool> gRunning{false};

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

// NB: this check must *not* implicitly create the event loop!
// In fact, this is actually called inside the EventLoop constructor!
bool isCurrentThread() {
    return [NSThread isMainThread];
}

bool available() {
    return Cocoa::EventLoop::instance().available();
}

void poll(){
    // only on the main thread!
    if (isCurrentThread()){
        NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
        while (true) {
            NSEvent *event = [NSApp
                nextEventMatchingMask:NSAnyEventMask
                untilDate:nil
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
            LOG_WARNING("Cocoa: NSApp already initialized!");
        } else {
            // NSApp will automatically point to the NSApplication singleton
            [NSApplication sharedApplication];
            LOG_DEBUG("Cocoa: init event loop (polling)");
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
        LOG_DEBUG("Cocoa: init event loop");
    }

    proxy_ = [[EventLoopProxy alloc] initWithOwner:this];

    LOG_DEBUG("Cocoa: UI thread ready");
}

EventLoop::~EventLoop(){
    if (haveNSApp_) {
        if (timer_) {
            [timer_ invalidate];
            timer_ = nil;
        }
        [proxy_ release];
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
        LOG_DEBUG("Cocoa: callSync() failed - no NSApp");
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
        LOG_DEBUG("Cocoa: callAsync() failed - no NSApp");
        return false;
    }
}

void EventLoop::startPolling() {
    if (timer_) {
        LOG_ERROR("EventLoop: poll function timer already installed!");
        return;
    }
    timer_ = [NSTimer scheduledTimerWithTimeInterval:(updateIntervalMillis * 0.001)
                target:proxy_
                selector:@selector(poll)
                userInfo:nil
                repeats:YES];
}

void EventLoop::stopPolling() {
    if (timer_) {
        [timer_ invalidate];
        timer_ = nil;
    }
}

/*///////////////// Window ///////////////////////*/

std::atomic<int> Window::numWindows_{0};

Window::Window(IPlugin& plugin)
    : plugin_(&plugin) {}

Window::~Window(){
    doClose();
    LOG_DEBUG("Cocoa: destroyed Window");
}

bool Window::canResize() const {
    return plugin_->info().editorResizable();
}

void Window::open(){
    UIThread::callAsync([](void *x){
        static_cast<Window *>(x)->doOpen();
    }, this);
}

// to be called on the main thread
void Window::doOpen(){
    LOG_DEBUG("Cocoa: open window");

    if (window_){
        // just bring to top
        [NSApp activateIgnoringOtherApps:YES];
        [window_ makeKeyAndOrderFront:nil];
        LOG_DEBUG("Cocoa: restore");
        return;
    }

    NSRect frame = NSMakeRect(0, 0, 200, 200);
    NSUInteger style = NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask;
    if (canResize()) {
        style |= NSResizableWindowMask;
        LOG_DEBUG("Cocoa: can resize");
    }
    window_ = [[CocoaEditorWindow alloc] initWithContentRect:frame
                styleMask:style
                backing:NSBackingStoreBuffered
                defer:NO];
    if (window_){
        [window_ setOwner:this];
        [[NSNotificationCenter defaultCenter] addObserver:window_ selector:@selector(windowDidMove:)
                name:NSWindowDidMoveNotification object:window_];
        [[NSNotificationCenter defaultCenter] addObserver:window_ selector:@selector(windowDidResize:)
                name:NSWindowDidResizeNotification object:window_];
        
        // set window title
        NSString *title = @(plugin_->info().name.c_str());
        [window_ setTitle:title];
        LOG_DEBUG("Cocoa: created Window");

        // set window coordinates
        bool didOpenEditor = false;
        if (rect_.valid()){
            LOG_DEBUG("Cocoa: restore editor rect");
            restoring_ = true; // see resize()
        } else {
            // get window dimensions from plugin
            Rect r;
            if (!plugin_->getEditorRect(r)) {
                // HACK for plugins which don't report the window size
                // without the editor being opened
                LOG_DEBUG("Cocoa: couldn't get editor rect!");
                plugin_->openEditor(getHandle());
                plugin_->getEditorRect(r);
                didOpenEditor = true;
            }
            LOG_DEBUG("Cocoa: editor size " << r.w << " * " << r.h);
            // adjust initial position and size!
            rect_.w = r.w;
            rect_.h = r.h;
        }

        updateGeometry();

        if (!didOpenEditor){
            plugin_->openEditor(getHandle());
        }

        timer_ = [NSTimer scheduledTimerWithTimeInterval:(EventLoop::updateIntervalMillis * 0.001)
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
        restoring_ = false;
    }
}

void Window::close(){
    EventLoop::instance().callAsync([](void *x){
        static_cast<Window *>(x)->doClose();
    }, this);
}

// to be called on the main thread
void Window::doClose() {
    if (window_) {
        LOG_DEBUG("Cocoa: close window");
        // to distinguish from manually closing the window, see onClose().
        closing_ = true;
        // will implicitly call onClose()!
        [window_ performClose:nil];
        closing_ = false;
    }
}

// to be called on the main thread
void Window::onClose() {
    if (window_) {
        [[NSNotificationCenter defaultCenter] removeObserver:window_ name:NSWindowDidMoveNotification object:window_];
        [[NSNotificationCenter defaultCenter] removeObserver:window_ name:NSWindowDidResizeNotification object:window_];

        [timer_ invalidate];
        timer_ = nil;

        plugin_->closeEditor();

        if (!closing_) {
            // window has been closed manually
            if (auto listener = plugin_->getListener()) {
                listener->editorClosed();
            }
        }

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

void Window::updateGeometry() {
    LOG_DEBUG("Cocoa: update geometry: " << rect_.x << ", " << rect_.y
              << ", " << rect_.w << " x " << rect_.h);

    // first adjust the size because we need it to adjust the position!
    NSRect content = NSMakeRect(rect_.x, rect_.y, rect_.w, rect_.h);
    NSRect frame = [window_ frameRectForContentRect:content];

    // now move the window to the given x coordinate
    // NB: ignore move event, see onMove().
    ignoreMove_ = true;
    [window_ setFrameOrigin:NSMakePoint(rect_.x, rect_.y)];
    ignoreMove_ = false;
    // then obtain the screen height
    auto screenHeight = window_.screen.frame.size.height;
    // finally bash size and flip y coordinate
    frame.origin.x = rect_.x;
    frame.origin.y = screenHeight - (rect_.y + frame.size.height);

    LOG_DEBUG("Cocoa: setFrame: " << frame.origin.x << ", " << frame.origin.y
              << ", " << frame.size.width << " x " << frame.size.height);
    [window_ setFrame:frame display:YES];

    // TODO: verify position and size
    if (rect_.x != lastRect_.x || rect_.y != lastRect_.y) {
        if (auto listener = plugin_->getListener()) {
            listener->editorMoved(rect_.x, rect_.y);
        }
        lastRect_.x = rect_.x;
        lastRect_.y = rect_.y;
    }
    if (rect_.w != lastRect_.w || rect_.h != lastRect_.h) {
        if (auto listener = plugin_->getListener()) {
            listener->editorResized(rect_.w, rect_.h);
        }
        lastRect_.w = rect_.w;
        lastRect_.h = rect_.h;
    }
}

void Window::setPos(int x, int y){
    EventLoop::instance().callAsync([](void *user){
        auto cmd = static_cast<Command *>(user);
        auto owner = cmd->owner;
        // save position
        owner->rect_.x = cmd->x;
        owner->rect_.y = cmd->y;
        if (owner->getHandle()){
            owner->updateGeometry();
        }
        delete cmd;
    }, new Command { this, x, y });
}

void Window::setSize(int w, int h){
    LOG_DEBUG("Cocoa: setSize: " << w << ", " << h);
    if (w > 0 && h > 0){
        EventLoop::instance().callAsync([](void *user){
            auto cmd = static_cast<Command *>(user);
            auto w = cmd->x;
            auto h = cmd->y;
            auto owner = cmd->owner;
            // only if we can resize!
            if (owner->canResize()) {
                // save size
                owner->rect_.w = w;
                owner->rect_.h = h;
                if (owner->getHandle()){
                    owner->updateGeometry();
                }
            }
            delete cmd;
        }, new Command { this, w, h });
    }
}

void Window::onMove(int x, int y) {
    LOG_DEBUG("Cocoa: onMove: " << x << ", " << y);
    if (!ignoreMove_) {
        // save position
        rect_.x = x;
        rect_.y = y;
        if (x != lastRect_.x || y != lastRect_.y) {
            if (auto listener = plugin_->getListener()) {
                listener->editorMoved(x, y);
            }
            lastRect_.x = x;
            lastRect_.y = y;
        }
    } else {
        LOG_DEBUG("Cocoa: ignore move");
    }
}

void Window::onResize(int w, int h) {
    LOG_DEBUG("Cocoa: onResize: " << w << ", " << h);
    if (canResize()) {
        plugin_->resizeEditor(w, h);
    }
    // save size
    rect_.w = w;
    rect_.h = h;
    if (w != lastRect_.w || h != lastRect_.h) {
        if (auto listener = plugin_->getListener()) {
            listener->editorResized(w, h);
        }
        lastRect_.w = w;
        lastRect_.h = h;
    }
}

void Window::resize(int w, int h) {
    // the workaround above creates problems with certain plugins (e.g. Helm.vst3) and
    // it doesn't seem to be necessary for certain other plugins (e.g. Surge XT.vst3),
    // so let's disable it for now.
#if 0
    // ignore resize requests when restoring a resizable plugin.
    // NB: this doesn't seem to work with "resizable" VST2 plugins.
    if (restoring_ && canResize()) {
        LOG_DEBUG("Cocoa: ignore resize request while restoring window");
        return;
    }
#endif

    LOG_DEBUG("Cocoa: resized by plugin: " << w << ", " << h);
    rect_.w = w;
    rect_.h = h;
    updateGeometry();
}

} // Cocoa

IWindow::ptr IWindow::create(IPlugin &plugin){
    return std::make_unique<Cocoa::Window>(plugin);
}

} // vst
