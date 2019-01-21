#import "VSTWindowCocoa.h"
#include "Utility.h"

#if __has_feature(objc_arc)
#error This file must be compiled without ARC!
#endif

#include <iostream>

@implementation VSTEditorWindow {
}

@synthesize plugin = _plugin;

- (BOOL)windowShouldClose:(id)sender {
    LOG_DEBUG("window should close");
    [self orderOut:NSApp];
    IVSTPlugin *plugin = [self plugin];
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

namespace VSTWindowFactory {
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
    IVSTWindow* createCocoa(IVSTPlugin *plugin) {
        return new VSTWindowCocoa(plugin);
    }
    void mainLoopPollCocoa(){
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

VSTWindowCocoa::VSTWindowCocoa(IVSTPlugin *plugin){
    std::cout << "try opening VSTWindowCocoa" << std::endl;
    
    NSRect frame = NSMakeRect(0, 0, 200, 200);
    VSTEditorWindow *window = [[VSTEditorWindow alloc] initWithContentRect:frame
                styleMask:(NSTitledWindowMask | NSClosableWindowMask 
                | NSMiniaturizableWindowMask) 
                backing:NSBackingStoreBuffered
                defer:NO];
    if (window){
        [window setPlugin:plugin];
        window_ = window;
        std::cout << "created VSTWindowCocoa" << std::endl;
    }
}

VSTWindowCocoa::~VSTWindowCocoa(){
        // close the editor *before* the window is destroyed.
        // the destructor (like any other method of VSTWindowCocoa) must be called in the main thread
    IVSTPlugin *plugin = [window_ plugin];
    plugin->closeEditor();
    [window_ close];
    [window_ release];
    std::cout << "destroyed VSTWindowCocoa" << std::endl;
}

void * VSTWindowCocoa::getHandle(){
    return [window_ contentView];
}

void VSTWindowCocoa::run(){}

void VSTWindowCocoa::setTitle(const std::string& title){
    NSString *name = @(title.c_str());
    [window_ setTitle:name];
}

void VSTWindowCocoa::setGeometry(int left, int top, int right, int bottom){
        // in CoreGraphics y coordinates are "flipped" (0 = bottom)
    NSRect content = NSMakeRect(left, bottom, right-left, bottom-top);
    NSRect frame = [window_  frameRectForContentRect:content];
    [window_ setFrame:frame display:YES];
}

void VSTWindowCocoa::show(){
    [window_ makeKeyAndOrderFront:nil];
    IVSTPlugin *plugin = [window_ plugin];
    /* we open/close the editor on demand to make sure no unwanted
     * GUI drawing is happening behind the scenes
     * (for some reason, on macOS parameter automation would cause
     * redraws even if the window is hidden)
    */
    plugin->openEditor(getHandle());
}

void VSTWindowCocoa::hide(){
    [window_ orderOut:nil];
    IVSTPlugin *plugin = [window_ plugin];
    plugin->closeEditor();
}

void VSTWindowCocoa::minimize(){
    hide();
}

void VSTWindowCocoa::restore(){
    show();
}

void VSTWindowCocoa::bringToTop(){
    restore();
}
