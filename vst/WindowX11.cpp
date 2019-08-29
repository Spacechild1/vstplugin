#include "WindowX11.h"
#include "Utility.h"

#include <cstring>

namespace vst {
namespace X11 {

namespace UIThread {

#if !HAVE_UI_THREAD
#error "HAVE_UI_THREAD must be defined for X11!"
// void poll(){}
#endif

Atom EventLoop::wmProtocols;
Atom EventLoop::wmDelete;
Atom EventLoop::wmQuit;
Atom EventLoop::wmCreatePlugin;
Atom EventLoop::wmDestroyPlugin;

IPlugin::ptr create(const PluginInfo& info){
    return EventLoop::instance().create(info);
}

void destroy(IPlugin::ptr plugin){
    EventLoop::instance().destroy(std::move(plugin));
}


bool check(){
    return std::this_thread::get_id() == EventLoop::instance().threadID();
}

EventLoop& EventLoop::instance(){
    static EventLoop thread;
    return thread;
}

EventLoop::EventLoop() {
    if (!XInitThreads()){
        LOG_WARNING("XInitThreads failed!");
    }
    display_ = XOpenDisplay(NULL);
	if (!display_){
        throw Error("X11: couldn't open display!");
	}
#if 0
    // root_ = DefaultRootWindow(display_);
#else
    // for some reason, the "real" root window doesn't receive
    // client messages, so I have to create a dummy window instead...
	root_ = XCreateSimpleWindow(display_, DefaultRootWindow(display_),
				0, 0, 1, 1, 1, 0, 0);
#endif
    wmProtocols = XInternAtom(display_, "WM_PROTOCOLS", 0);
    wmDelete = XInternAtom(display_, "WM_DELETE_WINDOW", 0);
    // custom messages
    wmQuit = XInternAtom(display_, "WM_QUIT", 0);
    wmCreatePlugin = XInternAtom(display_, "WM_CREATE_PLUGIN", 0);
    wmDestroyPlugin = XInternAtom(display_, "WM_DESTROY_PLUGIN", 0);
    // run thread
    thread_ = std::thread(&EventLoop::run, this);
    LOG_DEBUG("X11: UI thread ready");
}

EventLoop::~EventLoop(){
    if (thread_.joinable()){
        // post quit message
        // https://stackoverflow.com/questions/10792361/how-do-i-gracefully-exit-an-x11-event-loop
        postClientEvent(wmQuit);
        // wait for thread to finish
        thread_.join();
        LOG_VERBOSE("X11: terminated UI thread");
    }
    if (display_){
    #if 1
        // destroy dummy window
        XDestroyWindow(display_, root_);
    #endif
        XCloseDisplay(display_);
    }
}

void EventLoop::run(){
    XEvent event;
    LOG_DEBUG("X11: start event loop");
    while (true){
	    XNextEvent(display_, &event);
        LOG_DEBUG("got event: " << event.type);
	    if (event.type == ClientMessage){
			auto& msg = event.xclient;
            auto type = msg.message_type;
            if (type == wmProtocols){
                if ((Atom)msg.data.l[0] == wmDelete){
                    // only hide window
                    XUnmapWindow(display_, msg.window);
                }
            } else if (type == wmCreatePlugin){
                LOG_DEBUG("wmCreatePlugin");
                std::unique_lock<std::mutex> lock(mutex_);
                try {
                    auto plugin = info_->create();
                    if (plugin->info().hasEditor()){
                        plugin->setWindow(std::make_unique<Window>(*display_, *plugin));
                    }
                    plugin_ = std::move(plugin);
                } catch (const Error& e){
                    err_ = e;
                    plugin_ = nullptr;
                }
                info_ = nullptr; // done
                lock.unlock();
                cond_.notify_one();
            } else if (type == wmDestroyPlugin){
                LOG_DEBUG("WM_DESTROY_PLUGIN");
                std::unique_lock<std::mutex> lock(mutex_);
                auto plugin = std::move(plugin_);
                // goes out of scope
                lock.unlock();
                cond_.notify_one();
            } else if (type == wmQuit){
                LOG_DEBUG("X11: quit event loop");
                break;
			} else {
                LOG_DEBUG("X11: unknown client message");
			}
		}
	}
}

bool EventLoop::postClientEvent(Atom atom){
    XClientMessageEvent event;
    memset(&event, 0, sizeof(XClientMessageEvent));
    event.type = ClientMessage;
    event.message_type = atom;
    event.format = 32;
    if (XSendEvent(display_, root_, 0, 0, (XEvent*)&event) != 0){
        if (XFlush(display_) != 0){
            return true;
        }
    }
    return false;
}

IPlugin::ptr EventLoop::create(const PluginInfo& info){
    if (thread_.joinable()){
        LOG_DEBUG("create plugin in UI thread");
        std::unique_lock<std::mutex> lock(mutex_);
        info_ = &info;
        // notify thread
        if (postClientEvent(wmCreatePlugin)){
            // wait till done
            LOG_DEBUG("waiting...");
            cond_.wait(lock, [&]{return info_ == nullptr; });
            if (plugin_){
                LOG_DEBUG("done");
                return std::move(plugin_);
            } else {
                throw err_;
            }
        } else {
            throw Error("X11: couldn't post to thread!");
        }
    } else {
        throw Error("X11: no UI thread!");
    }
}

void EventLoop::destroy(IPlugin::ptr plugin){
    if (thread_.joinable()){
        std::unique_lock<std::mutex> lock(mutex_);
        plugin_ = std::move(plugin);
        // notify thread
        if (postClientEvent(wmDestroyPlugin)){
            // wait till done
            cond_.wait(lock, [&]{return plugin_ == nullptr;});
        } else {
            throw Error("X11: couldn't post to thread!");
        }
    } else {
        throw Error("X11: no UI thread!");
    }
}

} // UIThread

Window::Window(Display& display, IPlugin& plugin)
    : display_(&display), plugin_(&plugin)
{
	int s = DefaultScreen(display_);
	window_ = XCreateSimpleWindow(display_, RootWindow(display_, s),
				10, 10, 100, 100,
				1, BlackPixel(display_, s), WhitePixel(display_, s));
#if 0
    XSelectInput(display_, window_, 0xffff);
#endif
		// intercept request to delete window when being closed
    XSetWMProtocols(display_, window_, &UIThread::EventLoop::wmDelete, 1);

    XClassHint *ch = XAllocClassHint();
    if (ch){
		ch->res_name = (char *)"VST Editor";
		ch->res_class = (char *)"VST Editor Window";
		XSetClassHint(display_, window_, ch);
		XFree(ch);
	}
    LOG_DEBUG("X11: created Window");
    setTitle(plugin_->info().name);
    int left = 100, top = 100, right = 400, bottom = 400;
    plugin_->getEditorRect(left, top, right, bottom);
    setGeometry(left, top, right, bottom);
    plugin_->openEditor(getHandle());
}

Window::~Window(){
    plugin_->closeEditor();
    XDestroyWindow(display_, window_);
    LOG_DEBUG("X11: destroyed Window");
}

void Window::setTitle(const std::string& title){
	XStoreName(display_, window_, title.c_str());
	XSetIconName(display_, window_, title.c_str());
	XFlush(display_);
    LOG_DEBUG("Window::setTitle: " << title);
}

void Window::setGeometry(int left, int top, int right, int bottom){
	XMoveResizeWindow(display_, window_, left, top, right-left, bottom-top);
	XFlush(display_);
    LOG_DEBUG("Window::setGeometry: " << left << " " << top << " "
        << right << " " << bottom);
}

void Window::show(){
	XMapWindow(display_, window_);
	XFlush(display_);
}

void Window::hide(){
	XUnmapWindow(display_, window_);
	XFlush(display_);
}

void Window::minimize(){
#if 0
	XIconifyWindow(display_, window_, DefaultScreen(display_));
#else
	XUnmapWindow(display_, window_);
#endif
	XFlush(display_);
}

void Window::restore(){
		// LATER find out how to really restore
	XMapWindow(display_, window_);
	XFlush(display_);
}

void Window::bringToTop(){
	minimize();
	restore();
    LOG_DEBUG("Window::bringToTop");
}

} // X11
} // vst
