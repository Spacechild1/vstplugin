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
    wmProtocols = XInternAtom(display_, "WM_PROTOCOLS", 0);
    wmDelete = XInternAtom(display_, "WM_DELETE_WINDOW", 0);
    // custom messages
    wmQuit = XInternAtom(display_, "WM_QUIT", 0);
    wmCreatePlugin = XInternAtom(display_, "WM_CREATE_PLUGIN", 0);
    wmDestroyPlugin = XInternAtom(display_, "WM_DESTROY_PLUGIN", 0);
    wmOpenEditor = XInternAtom(display_, "WM_OPEN_EDITOR", 0);
    wmCloseEditor = XInternAtom(display_, "WM_CLOSE_EDITOR", 0);
    wmUpdateEditor = XInternAtom(display_, "WM_UPDATE_EDITOR", 0);
    wmSetEditorPos = XInternAtom(display_, "WM_SET_EDITOR_POS", 0);
    wmSetEditorSize = XInternAtom(display_, "WM_SET_EDITOR_SIZE", 0);
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
                    auto it = pluginMap_.find(msg.window);
                    if (it != pluginMap_.end()){
                        it->second->closeEditor();
                        XUnmapWindow(display_, msg.window);
                    } else {
                        LOG_ERROR("bug wmDelete");
                    }
                }
            } else if (type == wmCreatePlugin){
                LOG_DEBUG("wmCreatePlugin");
                auto data = (PluginData *)msg.data.l[0];
                try {
                    auto plugin = data->info->create();
                    if (plugin->info().hasEditor()){
                        plugin->setWindow(std::make_unique<Window>(*display_, *plugin));
                    }
                    pluginMap_[msg.window] = plugin.get();
                    data->plugin = std::move(plugin);
                } catch (const Error& e){
                    data->err = e;
                }
                notify();
            } else if (type == wmDestroyPlugin){
                LOG_DEBUG("WM_DESTROY_PLUGIN");
                auto data = (PluginData *)msg.data.l[0];
                pluginMap_.erase(msg.window);
                data->plugin = nullptr;
                notify();
            } else if (type == wmOpenEditor){
                XMapWindow(display_, msg.window);
                XFlush(display_);
                auto plugin = (IPlugin *)msg.data.l[0];
                plugin->openEditor((void *)msg.window);
            } else if (type == wmCloseEditor){
                auto plugin = (IPlugin *)msg.data.l[0];
                plugin->closeEditor();
                XUnmapWindow(display_, msg.window);
                XFlush(display_);
            } else if (type == wmUpdateEditor){
                auto plugin = (IPlugin *)msg.data.l[0];
                plugin->updateEditor();
            } else if (type == wmSetEditorPos){
                auto x = msg.data.l[0];
                auto y = msg.data.l[1];
                XMoveWindow(display_, msg.window, x, y);
                XFlush(display_);
            } else if (type == wmSetEditorSize){
                auto w = msg.data.l[0];
                auto h = msg.data.l[1];
                XResizeWindow(display_, msg.window, w, h);
                XFlush(display_);
            } else if (type == wmQuit){
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
        auto start = std::chrono::high_resolution_clock::now();
        for (auto& it : pluginMap_){
            postClientEvent(root_, wmUpdateEditor, (long)it.second);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - start;
        std::this_thread::sleep_for(std::chrono::duration<double>(updateInterval) - elapsed);
    }
}

bool EventLoop::postClientEvent(::Window window, Atom atom, long data1, long data2){
    XClientMessageEvent event;
    memset(&event, 0, sizeof(XClientMessageEvent));
    event.type = ClientMessage;
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
    cond_.notify_one();
}

IPlugin::ptr EventLoop::create(const PluginInfo& info){
    if (thread_.joinable()){
        LOG_DEBUG("create plugin in UI thread");
        PluginData data;
        data.info = &info;
        // notify thread
        if (sendClientEvent(root_, wmCreatePlugin, (long)&info)){
            if (data.plugin){
                LOG_DEBUG("done");
                return std::move(data.plugin);
            } else {
                throw data.err;
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
        PluginData data;
        data.plugin = std::move(plugin);
        // notify thread
        if (!sendClientEvent(root_, wmDestroyPlugin, (long)&data)){
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
    XSetWMProtocols(display_, window_, &wmDelete, 1);

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

void Window::open(){
    UIThread::EventLoop::instance().postClientEvent(window_, wmOpenEditor, (long)plugin_);
}

void Window::close(){
    UIThread::EventLoop::instance().postClientEvent(window_, wmCloseEditor, (long)plugin_);
}

void Window::setPos(int x, int y){
    UIThread::EventLoop::instance().postClientEvent(window_, x, y);
}

void Window::setSize(int w, int h){
    UIThread::EventLoop::instance().postClientEvent(window_, w, h);
}

} // X11
} // vst
