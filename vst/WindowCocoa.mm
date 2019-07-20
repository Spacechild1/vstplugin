#import "WindowCocoa.h"
#include "Utility.h"

#if __has_feature(objc_arc)
#error This file must be compiled without ARC!
#endif

#include <iostream>


@implementation VSTEditorWindow {}

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

namespace WindowFactory {
    void initializeCocoa(){
        static bool initialized = false;
        if (!initialized){
                // make foreground application
            ProcessSerialNumber psn = {0, kCurrentProcess};
            TransformProcessType(&psn, kProcessTransformToForegroundApplication);
                // check if someone already created NSApp (just out of curiousity)
            if (NSApp != nullptr){
                LOG_WARNING("NSApp already initialized!");
                return;
            }
                // NSApp will automatically point to the NSApplication singleton
            [NSApplication sharedApplication];
            initialized = true;
        }
    }
    IWindow::ptr createCocoa(IPlugin::ptr plugin) {
        return std::make_shared<WindowCocoa>(std::move(plugin));
    }
    void pollCocoa(){
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

WindowCocoa::WindowCocoa(IPlugin::ptr plugin)
    : plugin_(std::move(plugin))
{
    LOG_DEBUG("try opening WindowCocoa");
    
    NSRect frame = NSMakeRect(0, 0, 200, 200);
    VSTEditorWindow *window = [[VSTEditorWindow alloc] initWithContentRect:frame
                styleMask:(NSTitledWindowMask | NSClosableWindowMask 
                | NSMiniaturizableWindowMask) 
                backing:NSBackingStoreBuffered
                defer:NO];
    if (window){
        [window setPlugin:plugin.get()];
        window_ = window;
        LOG_DEBUG("created WindowCocoa");
    }
}

WindowCocoa::~WindowCocoa(){
        // close the editor *before* the window is destroyed.
        // the destructor (like any other method of WindowCocoa) must be called in the main thread
    plugin_->closeEditor();
    [window_ close];
    [window_ release];
    LOG_DEBUG("destroyed WindowCocoa");
}

void * WindowCocoa::getHandle(){
    return [window_ contentView];
}

void WindowCocoa::run(){}

void WindowCocoa::quit(){}

void WindowCocoa::setTitle(const std::string& title){
    NSString *name = @(title.c_str());
    [window_ setTitle:name];
}

void WindowCocoa::setGeometry(int left, int top, int right, int bottom){
        // in CoreGraphics y coordinates are "flipped" (0 = bottom)
    NSRect content = NSMakeRect(left, bottom, right-left, bottom-top);
    NSRect frame = [window_  frameRectForContentRect:content];
    [window_ setFrame:frame display:YES];
}

void WindowCocoa::show(){
    [window_ makeKeyAndOrderFront:nil];
    /* we open/close the editor on demand to make sure no unwanted
     * GUI drawing is happening behind the scenes
     * (for some reason, on macOS parameter automation would cause
     * redraws even if the window is hidden)
    */
    plugin_->openEditor(getHandle());
}

void WindowCocoa::hide(){
    [window_ orderOut:nil];
    plugin_->closeEditor();
}

void WindowCocoa::minimize(){
    hide();
}

void WindowCocoa::restore(){
    show();
}

void WindowCocoa::bringToTop(){
    restore();
}

} // vst
