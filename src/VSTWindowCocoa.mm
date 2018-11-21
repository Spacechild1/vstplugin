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
        bool loop = true;
        do {
            NSAutoreleasePool *pool =[[NSAutoreleasePool alloc] init];
            NSEvent *event = [NSApp 
                nextEventMatchingMask:NSAnyEventMask
                untilDate:[[NSDate alloc] init]
                inMode:NSDefaultRunLoopMode
                dequeue:YES];
            if (!event){
                loop = false;
            } else {
                LOG_DEBUG("got event: " << [event type]);
            }
            [NSApp sendEvent:event];
            [NSApp updateWindows];
            [pool release];
        } while (loop);
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
    [window_ makeKeyAndOrderFront:NSApp];
    IVSTPlugin *plugin = [window_ plugin];
    plugin->openEditor(getHandle());
}

void VSTWindowCocoa::hide(){
    [window_ orderOut:NSApp];
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
