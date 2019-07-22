#import "WindowCocoa.h"
#include "Utility.h"

#if __has_feature(objc_arc)
#error This file must be compiled without ARC!
#endif

#include <iostream>


@implementation CocoaEditorWindow {}

@synthesize plugin = _plugin;

- (BOOL)windowShouldClose:(id)sender {
    LOG_DEBUG("window should close");
    [self orderOut:NSApp];
    vst::IPlugin *plugin = [self plugin];
    plugin->closeEditor();
    return NO;
}
/*
- (void)windowDidMiniaturize:(id)sender {
    LOG_DEBUG("window miniaturized");
}
- (void)windowDidDeminiaturize:(id)sender {
    LOG_DEBUG("window deminiaturized");    
}
*/
@end

namespace vst {
namespace Cocoa {

namespace UIThread {

#if !VSTTHREADS
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
#if !VSTTHREADS
    // create NSApplication and make it the foreground application
    ProcessSerialNumber psn = {0, kCurrentProcess};
    TransformProcessType(&psn, kProcessTransformToForegroundApplication);
    // check if someone already created NSApp (just out of curiousity)
    if (NSApp != nullptr){
        LOG_WARNING("NSApp already initialized!");
        return;
    }
        // NSApp will automatically point to the NSApplication singleton
    [NSApplication sharedApplication];
#endif
}

EventLoop::~EventLoop(){}

IPlugin::ptr EventLoop::create(const PluginInfo& info){
#if VSTTHREADS
    // todo
#else
    return doCreate(info);
#endif
}

IPlugin::ptr EventLoop::doCreate(const PluginInfo& info){
    auto plugin = info.create();
    if (info.hasEditor()){
        auto window = std::make_unique<Window>(*plugin);
        window->setTitle(info.name);
        int left, top, right, bottom;
        plugin->getEditorRect(left, top, right, bottom);
        window->setGeometry(left, top, right, bottom);
        plugin->openEditor(window->getHandle());
        plugin->setWindow(std::move(window));
    }
    return plugin;
}

void EventLoop::destroy(IPlugin::ptr plugin){
#if VSTTHREADS
    // todo
#else
    return doDestroy(std::move(plugin));
#endif
}

void EventLoop::doDestroy(IPlugin::ptr plugin){
    plugin->closeEditor();
    // goes out of scope
}

} // UIThread

Window::Window(IPlugin& plugin)
    : plugin_(&plugin)
{
    LOG_DEBUG("try opening Window");
    
    NSRect frame = NSMakeRect(0, 0, 200, 200);
    CocoaEditorWindow *window = [[CocoaEditorWindow alloc] initWithContentRect:frame
                styleMask:(NSTitledWindowMask | NSClosableWindowMask 
                | NSMiniaturizableWindowMask) 
                backing:NSBackingStoreBuffered
                defer:NO];
    if (window){
        [window setPlugin:plugin_];
        window_ = window;
        LOG_DEBUG("created Window");
    }
}

Window::~Window(){
        // close the editor *before* the window is destroyed.
        // the destructor (like any other method of Window) must be called in the main thread
    [window_ close];
    [window_ release];
    LOG_DEBUG("destroyed Window");
}

void * Window::getHandle(){
    return [window_ contentView];
}

void Window::setTitle(const std::string& title){
    NSString *name = @(title.c_str());
    [window_ setTitle:name];
}

void Window::setGeometry(int left, int top, int right, int bottom){
        // in CoreGraphics y coordinates are "flipped" (0 = bottom)
    NSRect content = NSMakeRect(left, bottom, right-left, bottom-top);
    NSRect frame = [window_  frameRectForContentRect:content];
    [window_ setFrame:frame display:YES];
}

void Window::show(){
    [window_ makeKeyAndOrderFront:nil];
#if !VSTTHREADS
    /* we open/close the editor on demand to make sure no unwanted
     * GUI drawing is happening behind the scenes
     * (for some reason, on macOS parameter automation would cause
     * redraws even if the window is hidden)
    */
    plugin_->openEditor(getHandle());
#endif
}

void Window::hide(){
    [window_ orderOut:nil];
#if !VSTTHREADS
    plugin_->closeEditor();
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
