#include "VSTWindowCocoa.h"

#include <iostream>

namespace VSTWindowFactory {
    void initializeCocoa(){
		
	}
    IVSTWindow* createCocoa() {
        return new VSTWindowCocoa();
    }
}

VSTWindowCocoa::VSTWindowCocoa(){
    std::cout << "try opening VSTWindowCocoa" << std::endl;
    [NSApplication sharedApplication];
    app_ = NSApp;
    NSRect frame = NSMakeRect(0, 0, 200, 200);
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                styleMask:NSTitledWindowMask
                backing:NSBackingStoreBuffered
                defer:NO];
    if (window){
        std::cout << "made window" << std::endl;
        [window setBackgroundColor:[NSColor blueColor]];
        
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
    if (window_){
        return window_.windowRef;
    } else {
        return nullptr;
    }
}

void VSTWindowCocoa::run(){
    return;
#if 1
// https://www.cocoawithlove.com/2009/01/demystifying-nsapplication-by.html
    NSAutoreleasePool *pool =[[NSAutoreleasePool alloc] init];
    
    [app_ finishLaunching];
    
    bool running = true;
    while (running){
        [pool release];
        pool =[[NSAutoreleasePool alloc] init];
        
        NSEvent *event = [app_ 
            nextEventMatchingMask:NSAnyEventMask
            untilDate:[NSDate distantFuture]
            inMode:NSDefaultRunLoopMode
            dequeue:YES];
        // std::cout << "got event" << std::endl;
        [app_ sendEvent:event];
        [app_ updateWindows];
    }
    
    [pool release];
#else
    while (true){
        usleep(1000);
    }
#endif
}

void VSTWindowCocoa::setTitle(const std::string& title){
    NSString *name = @(title.c_str());
    [window_ setTitle:name];
}

void VSTWindowCocoa::setGeometry(int left, int top, int right, int bottom){
        // in CoreGraphics y coordinates are "flipped" (0 = bottom)
    NSRect frame = NSMakeRect(left, bottom, right-left, bottom-top);
    [window_ setFrame:frame display:YES];
}

void VSTWindowCocoa::show(){
    [window_ makeKeyAndOrderFront:NSApp];
}

void VSTWindowCocoa::hide(){
    [window_ orderOut:NSApp];
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
