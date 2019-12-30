#include "WindowX11.h"
#include "Utility.h"

#include <cstring>

namespace vst {

// Plugin.cpp
void setThreadLowPriority();

namespace X11 {

namespace  {
Atom wmProtocols;
Atom wmDelete;
Atom wmQuit;
Atom wmCreatePlugin;
Atom wmDestroyPlugin;
Atom wmOpenEditor;
Atom wmCloseEditor;
Atom wmUpdatePlugins;
}

namespace UIThread {

#if !HAVE_UI_THREAD
#error "HAVE_UI_THREAD must be defined for X11!"
// void poll(){}
#endif

IPlugin::ptr create(const PluginInfo& info){
    return EventLoop::instance().create(info);
}

void destroy(IPlugin::ptr plugin){
    EventLoop::instance().destroy(std::move(plugin));
}

bool checkThread(){
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
    LOG_DEBUG("created X11 root window: " << root_);
    wmProtocols = XInternAtom(display_, "WM_PROTOCOLS", 0);
    wmDelete = XInternAtom(display_, "WM_DELETE_WINDOW", 0);
    // custom messages
    wmQuit = XInternAtom(display_, "WM_QUIT", 0);
    wmCreatePlugin = XInternAtom(display_, "WM_CREATE_PLUGIN", 0);
    wmDestroyPlugin = XInternAtom(display_, "WM_DESTROY_PLUGIN", 0);
    wmOpenEditor = XInternAtom(display_, "WM_OPEN_EDITOR", 0);
    wmUpdatePlugins = XInternAtom(display_, "WM_UPDATE_PLUGINS", 0);
    wmCloseEditor = XInternAtom(display_, "WM_CLOSE_EDITOR", 0);
    // run thread
    thread_ = std::thread(&EventLoop::run, this);
    // run timer thread
    timerThread_ = std::thread(&EventLoop::updatePlugins, this);
    LOG_DEBUG("X11: UI thread ready");
}

EventLoop::~EventLoop(){
    if (thread_.joinable()){
        // post quit message
        // https://stackoverflow.com/questions/10792361/how-do-i-gracefully-exit-an-x11-event-loop
        postClientEvent(root_, wmQuit);
        // wait for thread to finish
        thread_.join();
        LOG_VERBOSE("X11: terminated UI thread");
    }
    if (timerThread_.joinable()){
        timerThreadRunning_ = false;
        timerThread_.join();
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
    setThreadLowPriority();

    XEvent event;
    LOG_DEBUG("X11: start event loop");
    while (true){
	    XNextEvent(display_, &event);
        // LOG_DEBUG("got event: " << event.type);
	    if (event.type == ClientMessage){
			auto& msg = event.xclient;
            auto type = msg.message_type;
            if (type == wmProtocols){
                if ((Atom)msg.data.l[0] == wmDelete){
                    // only hide window
                    auto it = pluginMap_.find(msg.window);
                    if (it != pluginMap_.end()){
                        static_cast<Window *>(it->second->getWindow())->doClose();
                    } else {
                        LOG_ERROR("bug wmDelete " << msg.window);
                    }
                }
            } else if (type == wmCreatePlugin){
                LOG_DEBUG("wmCreatePlugin");
                try {
                    auto plugin = data_.info->create();
                    if (plugin->info().hasEditor()){
                        auto window = std::make_unique<Window>(*display_, *plugin);
                        pluginMap_[(::Window)window->getHandle()] = plugin.get();
                        plugin->setWindow(std::move(window));
                    }
                    data_.plugin = std::move(plugin);
                } catch (const Error& e){
                    data_.err = e;
                }
                notify();
            } else if (type == wmDestroyPlugin){
                LOG_DEBUG("wmDestroyPlugin");
                auto window = data_.plugin->getWindow();
                if (window){
                    pluginMap_.erase((::Window)window->getHandle());
                }
                data_.plugin = nullptr;
                notify();
            } else if (type == wmOpenEditor){
                LOG_DEBUG("wmOpenEditor");
                auto it = pluginMap_.find(msg.window);
                if (it != pluginMap_.end()){
                    static_cast<Window *>(it->second->getWindow())->doOpen();
                } else {
                    LOG_ERROR("bug wmOpenEditor: " << msg.window);   
                }
            } else if (type == wmCloseEditor){
                LOG_DEBUG("wmCloseEditor");
                auto it = pluginMap_.find(msg.window);
                if (it != pluginMap_.end()){
                    static_cast<Window *>(it->second->getWindow())->doClose();
                } else {
                    LOG_ERROR("bug wmCloseEditor: " << msg.window);   
                }
            } else if (type == wmUpdatePlugins){
                for (auto& it : pluginMap_){
                    auto plugin = it.second;
                    if (plugin){
                        static_cast<Window *>(plugin->getWindow())->doUpdate();
                    } else {
                        LOG_ERROR("bug wmUpdatePlugins: " << it.first);
                    }
                }
            } else if (type == wmQuit){
                LOG_DEBUG("wmQuit");
                LOG_DEBUG("X11: quit event loop");
                break; // quit event loop
			} else {
                LOG_DEBUG("X11: unknown client message");
			}
		}
	}
}

void EventLoop::updatePlugins(){
    // this seems to be the easiest way to do it...
    while (timerThreadRunning_){
        postClientEvent(root_, wmUpdatePlugins);
        std::this_thread::sleep_for(std::chrono::milliseconds(updateInterval));
    }
}

bool EventLoop::postClientEvent(::Window window, Atom atom, long data1, long data2){
    XClientMessageEvent event;
    memset(&event, 0, sizeof(XClientMessageEvent));
    event.type = ClientMessage;
    event.window = window;
    event.message_type = atom;
    event.format = 32;
    event.data.l[0] = data1;
    event.data.l[1] = data2;
    if (XSendEvent(display_, window, 0, 0, (XEvent*)&event) != 0){
        if (XFlush(display_) != 0){
            return true;
        }
    }
    return false;
}

bool EventLoop::sendClientEvent(::Window window, Atom atom, long data1, long data2){
    std::unique_lock<std::mutex> lock(mutex_);
    ready_ = false;
    if (postClientEvent(window, atom, data1, data2)){
        LOG_DEBUG("waiting...");
        cond_.wait(lock, [&]{return ready_; });
        LOG_DEBUG("done");
        return true;
    } else {
        return false;
    }
}

void EventLoop::notify(){
    std::unique_lock<std::mutex> lock(mutex_);
    ready_ = true;
    lock.unlock();
    LOG_DEBUG("notify");
    cond_.notify_one();
}

IPlugin::ptr EventLoop::create(const PluginInfo& info){
    if (thread_.joinable()){
        LOG_DEBUG("create plugin in UI thread");
        data_.info = &info;
        data_.plugin = nullptr;
        // notify thread
        if (sendClientEvent(root_, wmCreatePlugin)){
            if (data_.plugin){
                LOG_DEBUG("done");
                return std::move(data_.plugin);
            } else {
                throw data_.err;
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
        data_.plugin = std::move(plugin);
        // notify thread
        if (!sendClientEvent(root_, wmDestroyPlugin)){
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
    int left = 100, top = 100, right = 400, bottom = 400;
    plugin_->getEditorRect(left, top, right, bottom);
    int s = DefaultScreen(display_);
	window_ = XCreateSimpleWindow(display_, RootWindow(display_, s),
				x_, y_, right-left, bottom-top,
				1, BlackPixel(display_, s), WhitePixel(display_, s));
#if 0
    XSelectInput(display_, window_, 0xffff);
#endif
		// intercept request to delete window when being closed
    XSetWMProtocols(display_, window_, &wmDelete, 1);

    XClassHint *ch = XAllocClassHint();
    if (ch){
		ch->res_name = (char *)"VST Editor";
		ch->res_class = (char *)"VST Editor Window";
		XSetClassHint(display_, window_, ch);
		XFree(ch);
	}
    LOG_DEBUG("X11: created Window " << window_);
    setTitle(plugin_->info().name);
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

void Window::open(){
    UIThread::EventLoop::instance().postClientEvent(window_, wmOpenEditor);
}

void Window::doOpen(){
    if (!mapped_){
        XMapRaised(display_, window_);
        plugin_->openEditor((void *)window_);
        // restore position
        XMoveWindow(display_, window_, x_, y_);
        mapped_ = true;
    }
}

void Window::close(){
    UIThread::EventLoop::instance().postClientEvent(window_, wmCloseEditor);
}

void Window::doClose(){
    if (mapped_){
        plugin_->closeEditor();
        ::Window child;
        XTranslateCoordinates(display_, window_, DefaultRootWindow(display_), 0, 0, &x_, &y_, &child);
        LOG_DEBUG("stored position: " << x_ << ", " << y_);
        XUnmapWindow(display_, window_);
        mapped_ = false;
    }
}

void Window::setPos(int x, int y){
    XMoveWindow(display_, window_, x, y);
    XFlush(display_);
}

void Window::setSize(int w, int h){
    XResizeWindow(display_, window_, w, h);
    XFlush(display_);
}

void Window::doUpdate(){
    if (mapped_){
        plugin_->updateEditor();
    }
}

} // X11
} // vst
