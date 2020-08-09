#include "WindowX11.h"
#include "Utility.h"

#include <cstring>

namespace vst {

namespace UIThread {

void setup(){}

bool isCurrentThread(){
    return X11::EventLoop::instance().checkThread();
}

void poll(){}

bool callSync(Callback cb, void *user){
    return X11::EventLoop::instance().callSync(cb, user);
}

bool callAsync(Callback cb, void *user){
    return X11::EventLoop::instance().callAsync(cb, user);
}

} // UIThread

namespace X11 {

namespace  {
Atom wmProtocols;
Atom wmDelete;
Atom wmQuit;
Atom wmCall;
Atom wmOpenEditor;
Atom wmCloseEditor;
Atom wmUpdatePlugins;
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
    wmCall = XInternAtom(display_, "WM_CALL", 0);
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
    setThreadPriority(Priority::Low);

    XEvent event;
    LOG_DEBUG("X11: start event loop");
    while (true){
        XNextEvent(display_, &event);
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
            } else if (type == wmCall){
                LOG_DEBUG("wmCall");
                UIThread::Callback cb;
                void *data;
                memcpy(&cb, msg.data.b, sizeof(cb));
                memcpy(&data, msg.data.b + sizeof(cb), sizeof(data));
                cb(data);

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
        } else if (event.type == ConfigureNotify){
            XConfigureEvent& xce = event.xconfigure;
            LOG_DEBUG("ConfigureNotify");
            auto it = pluginMap_.find(xce.window);
            if (it != pluginMap_.end()){
                static_cast<Window *>(it->second->getWindow())->onConfigure(
                            xce.x, xce.y, xce.width, xce.height);
            } else {
                LOG_ERROR("bug ConfigureNotify: " << xce.window);
            }
        } else {
            // LOG_DEBUG("got event: " << event.type);
        }
    }
}

void EventLoop::updatePlugins(){
    // X11 doesn't seem to have a timer API...

    setThreadPriority(Priority::Low);

    while (timerThreadRunning_){
        postClientEvent(root_, wmUpdatePlugins);
        std::this_thread::sleep_for(std::chrono::milliseconds(updateInterval));
    }
}

bool EventLoop::callSync(UIThread::Callback cb, void *user){
    char buf[sizeof(cb) + sizeof(user)];
    memcpy(buf, &cb, sizeof(cb));
    memcpy(buf + sizeof(cb), &user, sizeof(user));
    return sendClientEvent(root_, wmCall, buf, sizeof(buf));
}

bool EventLoop::callAsync(UIThread::Callback cb, void *user){
    char buf[sizeof(cb) + sizeof(user)];
    memcpy(buf, &cb, sizeof(cb));
    memcpy(buf + sizeof(cb), &user, sizeof(user));
    return postClientEvent(root_, wmCall, buf, sizeof(buf));
}

bool EventLoop::postClientEvent(::Window window, Atom atom,
                                const char *data, size_t size){
    XClientMessageEvent event;
    memset(&event, 0, sizeof(XClientMessageEvent));
    event.type = ClientMessage;
    event.window = window;
    event.message_type = atom;
    event.format = 8;
    memcpy(event.data.b, data, size);
    if (XSendEvent(display_, window, 0, 0, (XEvent*)&event) != 0){
        if (XFlush(display_) != 0){
            return true;
        }
    }
    return false;
}

bool EventLoop::sendClientEvent(::Window window, Atom atom,
                                const char *data, size_t size){
    std::lock_guard<std::mutex> lock(mutex_); // prevent concurrent access
    if (postClientEvent(window, atom, data, size)){
        LOG_DEBUG("waiting...");
        event_.wait();
        LOG_DEBUG("done");
        return true;
    } else {
        return false;
    }
}

bool EventLoop::checkThread(){
    return std::this_thread::get_id() == thread_.get_id();
}

void EventLoop::registerWindow(Window &w){
    pluginMap_[(::Window)w.getHandle()] = w.getPlugin();
}

void EventLoop::unregisterWindow(Window &w){
    pluginMap_.erase((::Window)w.getHandle());
}

void EventLoop::notify(){
    LOG_DEBUG("notify");
    event_.signal();
}

Window::Window(Display& display, IPlugin& plugin)
    : display_(&display), plugin_(&plugin)
{
    // get window coordinates
    int left = 100, top = 100, right = 400, bottom = 400;
    bool gotEditorRect = plugin_->getEditorRect(left, top, right, bottom);
    // create window
    int s = DefaultScreen(display_);
    window_ = XCreateSimpleWindow(display_, RootWindow(display_, s),
                left, top, right - left, bottom - top,
                1, BlackPixel(display_, s), WhitePixel(display_, s));
    if (!gotEditorRect){
        // HACK for plugins which don't report the window size without the editor being opened
        LOG_DEBUG("couldn't get editor rect!");
        plugin_->openEditor((void *)window_);
        plugin_->getEditorRect(left, top, right, bottom);
        plugin_->closeEditor();
    }
    x_ = left;
    y_ = top;
    width_ = right - left;
    height_ = bottom - top;
    // receive configure events
    XSelectInput(display_, window_, StructureNotifyMask);
    // intercept request to delete window when being closed
    XSetWMProtocols(display_, window_, &wmDelete, 1);
    // disable resizing
    if (!plugin_->canResize()){
        LOG_DEBUG("can't resize");
        XSizeHints *hints = XAllocSizeHints();
        if (hints){
            hints->flags = PMinSize | PMaxSize;
            hints->max_width = hints->min_width = width_;
            hints->min_height = hints->max_height = height_;
            XSetWMNormalHints(display_, window_, hints);
            XFree(hints);
        }
    } else {
        LOG_DEBUG("can resize");
    }
    // set window class hint
    XClassHint *ch = XAllocClassHint();
    if (ch){
        ch->res_name = (char *)"VST Editor";
        ch->res_class = (char *)"VST Editor Window";
        XSetClassHint(display_, window_, ch);
        XFree(ch);
    }
    LOG_DEBUG("X11: created Window " << window_);
    setTitle(plugin_->info().name);

    EventLoop::instance().registerWindow(*this);
}

Window::~Window(){
    EventLoop::instance().unregisterWindow(*this);

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
    EventLoop::instance().postClientEvent(window_, wmOpenEditor);
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
    EventLoop::instance().postClientEvent(window_, wmCloseEditor);
}

void Window::doClose(){
    if (mapped_){
        plugin_->closeEditor();
    #if 0
        ::Window child;
        XTranslateCoordinates(display_, window_, DefaultRootWindow(display_), 0, 0, &x_, &y_, &child);
        LOG_DEBUG("stored position: " << x_ << ", " << y_);
    #endif
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

void Window::onConfigure(int x, int y, int width, int height){
    if (width_ != width || height_ != height){
        if (plugin_->canResize()){
            plugin_->resizeEditor(width, height);
        }
        width_ = width;
        height_ = height;
    }
    // always store position!
    x_ = x;
    y_ = y;
}

} // X11

IWindow::ptr IWindow::create(IPlugin &plugin){
    auto display = X11::EventLoop::instance().getDisplay();
    if (display){
        return std::make_unique<X11::Window>(*display, plugin);
    } else {
        return nullptr;
    }
}

} // vst
