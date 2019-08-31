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

- (void)windowDidMiniaturize:(NSNotification *)notification {
    LOG_DEBUG("window miniaturized");
}
- (void)windowDidDeminiaturize:(NSNotification *)notification {
    LOG_DEBUG("window deminiaturized");    
}
- (void)windowDidMove:(NSNotification *)notification {
    LOG_DEBUG("window did move");    
}

@end

namespace vst {
namespace Cocoa {

namespace UIThread {

#if !HAVE_UI_THREAD
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

#if HAVE_UI_THREAD
bool checkThread(){
    return [NSThread isMainThread];
}
#endif

EventLoop& EventLoop::instance(){
    static EventLoop thread;
    return thread;
}

EventLoop::EventLoop(){
    // transform process into foreground application
    ProcessSerialNumber psn = {0, kCurrentProcess};
    TransformProcessType(&psn, kProcessTransformToForegroundApplication);
#if HAVE_UI_THREAD
    // we must access NSApp only once in the beginning
    haveNSApp_ = (NSApp != nullptr);
    LOG_DEBUG("init cocoa event loop");
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
        LOG_ERROR("EventLoop::destroy: can't dispatch to main thread - no NSApp!");
    }
    // fallback:
#endif
    return doCreate();
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
    } else {
        LOG_ERROR("EventLoop::destroy: can't dispatch to main thread - no NSApp!");
    }
#endif
    // plugin destroyed
}

} // UIThread

Window::Window(IPlugin& plugin)
    : plugin_(&plugin) {
    origin_ = NSMakePoint(100, 100); // default position
}

// the destructor must be called on the main thread!
Window::~Window(){
    if (window_){
        [window_ close];
    }
    LOG_DEBUG("destroyed Window");
}

// to be called on the main thread
void Window::doOpen(){
    if (window_){
        [window_ makeKeyAndOrderFront:nil];
        return;
    }
    
    LOG_DEBUG("try create Window");
    
    NSRect frame = NSMakeRect(0, 0, 200, 200);
    CocoaEditorWindow *window = [[CocoaEditorWindow alloc] initWithContentRect:frame
                styleMask:(NSTitledWindowMask | NSClosableWindowMask 
                | NSMiniaturizableWindowMask) 
                backing:NSBackingStoreBuffered
                defer:NO];
    if (window){
        [window setOwner:this];
        window_ = window;
        
        setTitle(plugin_->info().name);
        int left = 0, top = 0, right = 300, bottom = 300;
        plugin_->getEditorRect(left, top, right, bottom);
        setGeometry(left, top, right, bottom);
        
        [window_ setFrameOrigin:origin_];
        
        plugin_->openEditor(getHandle());
        
        [window_ makeKeyAndOrderFront:nil];
        LOG_DEBUG("created Window");
    }
}

// to be called on the main thread
void Window::onClose(){
    plugin_->closeEditor();
    origin_ = [window_ frame].origin;
    window_ = nullptr;
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

void Window::setGeometry(int left, int top, int right, int bottom){
    if (window_){
        // in CoreGraphics y coordinates are "flipped" (0 = bottom)
        NSRect content = NSMakeRect(left, bottom, right-left, bottom-top);
        NSRect frame = [window_  frameRectForContentRect:content];
        [window_ setFrame:frame display:YES];
    }
}

void Window::show(){
    LOG_DEBUG("show window");
#if HAVE_UI_THREAD
    dispatch_async(dispatch_get_main_queue(), ^{
        doOpen();
    });
#else
    doOpen();
#endif
}

void Window::hide(){
    LOG_DEBUG("hide window");
#if HAVE_UI_THREAD
    dispatch_async(dispatch_get_main_queue(), ^{
        [window_ performClose:nil];
    });
#else
    [window_ performClose:nil];
#endif
}

void Window::minimize(){
    hide();
}

void Window::restore(){
    show();
}

void Window::bringToTop(){
    restore();
}

} // Cocoa
} // vst
