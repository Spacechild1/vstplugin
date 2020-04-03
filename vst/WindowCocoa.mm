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
namespace Cocoa {

namespace UIThread {

#if HAVE_UI_THREAD
bool checkThread(){
    return [NSThread isMainThread];
}
#else
void poll(){
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
#endif

IPlugin::ptr create(const PluginInfo& info){
    return EventLoop::instance().create(info);
}

void destroy(IPlugin::ptr plugin){
    EventLoop::instance().destroy(std::move(plugin));
}

EventLoop& EventLoop::instance(){
    static EventLoop thread;
    return thread;
}

EventLoop::EventLoop(){
    // transform process into foreground application
    ProcessSerialNumber psn = {0, kCurrentProcess};
    TransformProcessType(&psn, kProcessTransformToForegroundApplication);
#if HAVE_UI_THREAD
    // we must access NSApp only once in the beginning (why?)
    haveNSApp_ = (NSApp != nullptr);
    if (haveNSApp_){
        LOG_DEBUG("init cocoa event loop");
    } else {
        LOG_WARNING("The host application doesn't have a UI thread (yet?), so I can't show the VST GUI editor.");
    }
#else
    // create NSApplication in this thread (= main thread) 
    // check if someone already created NSApp (just out of curiousity)
    if (NSApp != nullptr){
        LOG_WARNING("NSApp already initialized!");
        return;
    }
        // NSApp will automatically point to the NSApplication singleton
    [NSApplication sharedApplication];
    LOG_DEBUG("init cocoa event loop (polling)");
#endif
}

EventLoop::~EventLoop(){}

IPlugin::ptr EventLoop::create(const PluginInfo& info){
    auto doCreate = [&](){
        auto plugin = info.create();
        if (info.hasEditor()){
            plugin->setWindow(std::make_unique<Window>(*plugin));
        }
        return plugin;
    };
#if HAVE_UI_THREAD
    if (haveNSApp_){
        auto queue = dispatch_get_main_queue();
        __block IPlugin* plugin;
        __block Error err;
        LOG_DEBUG("dispatching...");
        dispatch_sync(queue, ^{
            try {
                plugin = doCreate().release();
            } catch (const Error& e){
                err = e;
            }
        });
        LOG_DEBUG("done!");
        if (plugin){
            return IPlugin::ptr(plugin);
        } else {
            throw err;
        }
    } else {
        return info.create(); // create without window in this thread
    }
#else
    return doCreate();
#endif
}

void EventLoop::destroy(IPlugin::ptr plugin){
#if HAVE_UI_THREAD
    if (haveNSApp_){
        auto queue = dispatch_get_main_queue();
        __block IPlugin* p = plugin.release();
        LOG_DEBUG("dispatching...");
        dispatch_sync(queue, ^{ delete p; });
        LOG_DEBUG("done!");
        return;
    }
#endif
    // plugin destroyed
}

} // UIThread

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
#if HAVE_UI_THREAD
    dispatch_async(dispatch_get_main_queue(), ^{
        doOpen();
    });
#else
    doOpen();
#endif
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

        timer_ = [NSTimer scheduledTimerWithTimeInterval:(UIThread::updateInterval * 0.001)
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
#if HAVE_UI_THREAD
    dispatch_async(dispatch_get_main_queue(), ^{
        [window_ performClose:nil];
    });
#else
    [window_ performClose:nil];
#endif
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
} // vst
