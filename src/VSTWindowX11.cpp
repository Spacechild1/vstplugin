#include "VSTWindowX11.h"

#include <iostream>
#include <cstring>

namespace VSTWindowFactory {
    void initializeX11(){
		if (!XInitThreads()){
			std::cout << "XInitThreads failed!" << std::endl;
		}
	}
}

VSTWindowX11::VSTWindowX11(){
	display_ = XOpenDisplay(NULL);
	if (!display_){
		std::cout << "couldn't open display" << std::endl;
		return;
	}
	int s = DefaultScreen(display_);
	window_ = XCreateSimpleWindow(display_, RootWindow(display_, s),
				10, 10, 100, 100,
				1, BlackPixel(display_, s), WhitePixel(display_, s));
#if 0
    XSelectInput(display_, window_, 0xffff);
#endif
	wmProtocols_ = XInternAtom(display_, "WM_PROTOCOLS", 0);
    wmDelete_ = XInternAtom(display_, "WM_DELETE_WINDOW", 0);
    wmQuit_ = XInternAtom(display_, "WM_QUIT", 0);
		// intercept request to delete window when being closed
    XSetWMProtocols(display_, window_, &wmDelete_, 1);

    XClassHint *ch = XAllocClassHint();
    if (ch){
		ch->res_name = (char *)"VST Editor";
		ch->res_class = (char *)"VST Editor Window";
		XSetClassHint(display_, window_, ch);
		XFree(ch);
	}

    std::cout << "created VSTWindowX11: " << window_ << std::endl;
}

VSTWindowX11::~VSTWindowX11(){
#if 0
	std::cout << "about to destroy VSTWindowX11" << std::endl;
	XDestroyWindow(display_, window_);
    std::cout << "destroyed VSTWindowX11" << std::endl;
#endif
		// post quit message
	XClientMessageEvent event;
	memset(&event, 0, sizeof(XClientMessageEvent));
	event.type = ClientMessage;
	event.message_type = wmQuit_;
	event.format = 32;
	XSendEvent(display_, window_, 0, 0, (XEvent*)&event);
    XFlush(display_);
}

void VSTWindowX11::run(){
	bool running = true;
	XEvent e;
    while (running){
	    XNextEvent(display_, &e);
			// https://stackoverflow.com/questions/10792361/how-do-i-gracefully-exit-an-x11-event-loop
	    if (e.type == ClientMessage){
			auto& msg = e.xclient;
			if (msg.message_type == wmProtocols_ && (Atom)msg.data.l[0] == wmDelete_){
				hide(); // only hide window
				std::cout << "window closed!" << std::endl;
			} else if (msg.message_type == wmQuit_){
				running = false; // quit event loop
				std::cout << "quit" << std::endl;
			} else {
				std::cout << "unknown client message" << std::endl;
			}
		}
	}
	std::cout << "closing display" << std::endl;
	XCloseDisplay(display_);
}

void VSTWindowX11::setTitle(const std::string& title){
	XStoreName(display_, window_, title.c_str());
	XSetIconName(display_, window_, title.c_str());
	XFlush(display_);
	std::cout << "VSTWindowX11::setTitle: " << title << std::endl;
}

void VSTWindowX11::setGeometry(int left, int top, int right, int bottom){
	XMoveResizeWindow(display_, window_, left, top, right-left, bottom-top);
	XFlush(display_);
	std::cout << "VSTWindowX11::setGeometry: " << left << " " << top << " "
		<< right << " " << bottom << std::endl;
}

void VSTWindowX11::show(){
	XMapWindow(display_, window_);
	XFlush(display_);
}

void VSTWindowX11::hide(){
	XUnmapWindow(display_, window_);
	XFlush(display_);
}

void VSTWindowX11::minimize(){
#if 0
	XIconifyWindow(display_, window_, DefaultScreen(display_));
#else
	XUnmapWindow(display_, window_);
#endif
	XFlush(display_);
}

void VSTWindowX11::restore(){
		// LATER find out how to really restore
	XMapWindow(display_, window_);
	XFlush(display_);
}

void VSTWindowX11::bringToTop(){
	minimize();
	restore();
    std::cout << "VSTWindowX11::bringToTop" << std::endl;
}

namespace VSTWindowFactory {
    IVSTWindow* createX11() {
        return new VSTWindowX11();
    }
}
