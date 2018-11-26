#include "VSTWindowX11.h"
#include "Utility.h"

#include <cstring>

namespace VSTWindowFactory {
    void initializeX11(){
        static bool initialized = false;
        if (!initialized){
            if (!XInitThreads()){
                LOG_WARNING("XInitThreads failed!");
            } else {
                initialized = true;
            }
        }
	}
    IVSTWindow* createX11() {
		return new VSTWindowX11();
    }
}

VSTWindowX11::VSTWindowX11(){
	display_ = XOpenDisplay(NULL);
	if (!display_){
		LOG_ERROR("VSTWindowX11: couldn't open display!");
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

    LOG_DEBUG("created VSTWindowX11: " << window_);
}

VSTWindowX11::~VSTWindowX11(){
		// post quit message
	XClientMessageEvent event;
	memset(&event, 0, sizeof(XClientMessageEvent));
	event.type = ClientMessage;
	event.message_type = wmQuit_;
	event.format = 32;
	XSendEvent(display_, window_, 0, 0, (XEvent*)&event);
    XFlush(display_);
    LOG_DEBUG("about to destroy VSTWindowX11");
	XDestroyWindow(display_, window_);
    LOG_DEBUG("destroyed VSTWindowX11");
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
                LOG_DEBUG("X11: window closed!");
			} else if (msg.message_type == wmQuit_){
				running = false; // quit event loop
                LOG_DEBUG("X11: quit");
			} else {
                LOG_DEBUG("X11: unknown client message");
			}
		}
	}
	XCloseDisplay(display_);
}

void VSTWindowX11::setTitle(const std::string& title){
	XStoreName(display_, window_, title.c_str());
	XSetIconName(display_, window_, title.c_str());
	XFlush(display_);
    LOG_DEBUG("VSTWindowX11::setTitle: " << title);
}

void VSTWindowX11::setGeometry(int left, int top, int right, int bottom){
	XMoveResizeWindow(display_, window_, left, top, right-left, bottom-top);
	XFlush(display_);
    LOG_DEBUG("VSTWindowX11::setGeometry: " << left << " " << top << " "
        << right << " " << bottom);
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
    LOG_DEBUG("VSTWindowX11::bringToTop");
}
