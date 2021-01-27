#include "WindowX11.h"
#include "Utility.h"

#include <cstring>

namespace vst {

namespace UIThread {

// fake event loop
Event gQuitEvent_;

void setup(){
    X11::EventLoop::instance();
}

void run(){
    gQuitEvent_.wait();
}

void quit(){
    gQuitEvent_.set();
}

bool isCurrentThread(){
    return X11::EventLoop::instance().checkThread();
}

bool available() { return true; }

void poll(){}

bool sync(){
    return X11::EventLoop::instance().sync();
}

bool callSync(Callback cb, void *user){
    return X11::EventLoop::instance().callSync(cb, user);
}

bool callAsync(Callback cb, void *user){
    return X11::EventLoop::instance().callAsync(cb, user);
}

int32_t addPollFunction(PollFunction fn, void *context){
    return X11::EventLoop::instance().addPollFunction(fn, context);
}

void removePollFunction(int32_t handle){
    return X11::EventLoop::instance().removePollFunction(handle);
}

} // UIThread

namespace X11 {

namespace  {
Atom wmProtocols;
Atom wmDelete;
Atom wmQuit;
Atom wmCall;
Atom wmSync;
Atom wmUpdatePlugins;
}

/*//////////////// EventLoop ////////////////*/

EventLoop& EventLoop::instance(){
    static EventLoop thread;
    return thread;
}

EventLoop::EventLoop() {
    if (!XInitThreads()){
        LOG_WARNING("XInitThreads failed!");
    }
    display_ = XOpenDisplay(nullptr);
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
    wmSync = XInternAtom(display_, "WM_SYNC", 0);
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
                    auto w = findWindow(msg.window);
                    if (w){
                        w->onClose();
                    } else {
                        LOG_ERROR("X11: WM_DELETE: couldn't find Window " << msg.window);
                    }
                }
            } else if (type == wmCall){
                LOG_DEBUG("wmCall");
                UIThread::Callback cb;
                void *data;
                memcpy(&cb, msg.data.b, sizeof(cb));
                memcpy(&data, msg.data.b + sizeof(cb), sizeof(data));
                cb(data);
            } else if (type == wmSync){
                LOG_DEBUG("wmSync");
                event_.set();
            } else if (type == wmUpdatePlugins){
                // update plugins
                for (auto& w : windows_){
                    w->onUpdate();
                }
                // call poll functions
                std::lock_guard<std::mutex> lock(pollFunctionMutex_);
                for (auto& it : pollFunctions_){
                    it.second();
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
            LOG_DEBUG("X11: ConfigureNotify");
            auto w = findWindow(xce.window);
            if (w){
                w->onConfigure(xce.x, xce.y, xce.width, xce.height);
            } else {
                LOG_ERROR("X11: ConfigureNotify: couldn't find Window " << xce.window);
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

Window* EventLoop::findWindow(::Window handle){
    for (auto& w : windows_){
        if (w->getHandle() == (void *)handle){
            return w;
        }
    }
    return nullptr;
}

bool EventLoop::sync(){
    if (UIThread::isCurrentThread()){
        return true;
    } else {
        std::lock_guard<std::mutex> lock(mutex_); // prevent concurrent calls
        if (postClientEvent(root_, wmSync)){
            LOG_DEBUG("waiting...");
            event_.wait();
            LOG_DEBUG("done");
            return true;
        } else {
            return false;
        }
    }
}

bool EventLoop::callSync(UIThread::Callback cb, void *user){
    if (UIThread::isCurrentThread()){
        cb(user);
        return true;
    } else {
        char buf[sizeof(cb) + sizeof(user)];
        memcpy(buf, &cb, sizeof(cb));
        memcpy(buf + sizeof(cb), &user, sizeof(user));

        std::lock_guard<std::mutex> lock(mutex_); // prevent concurrent access
        if (postClientEvent(root_, wmCall, buf, sizeof(buf))
                && postClientEvent(root_, wmSync))
        {
            LOG_DEBUG("waiting...");
            event_.wait();
            LOG_DEBUG("done");
            return true;
        } else {
            return false;
        }
    }
}

bool EventLoop::callAsync(UIThread::Callback cb, void *user){
    if (UIThread::isCurrentThread()){
        cb(user);
        return true;
    } else {
        char buf[sizeof(cb) + sizeof(user)];
        memcpy(buf, &cb, sizeof(cb));
        memcpy(buf + sizeof(cb), &user, sizeof(user));
        return postClientEvent(root_, wmCall, buf, sizeof(buf));
    }
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

UIThread::Handle EventLoop::addPollFunction(UIThread::PollFunction fn,
                                            void *context){
    std::lock_guard<std::mutex> lock(pollFunctionMutex_);
    auto handle = nextPollFunctionHandle_++;
    pollFunctions_.emplace(handle, [context, fn](){ fn(context); });
    return handle;
}

void EventLoop::removePollFunction(UIThread::Handle handle){
    std::lock_guard<std::mutex> lock(pollFunctionMutex_);
    pollFunctions_.erase(handle);
}

bool EventLoop::checkThread(){
    return std::this_thread::get_id() == thread_.get_id();
}

void EventLoop::registerWindow(Window *w){
    for (auto& it : windows_){
        if (it == w){
            LOG_ERROR("X11::EventLoop::registerWindow: bug");
            return;
        }
    }
    windows_.push_back(w);
}

void EventLoop::unregisterWindow(Window *w){
    for (auto it = windows_.begin(); it != windows_.end(); ++it){
        if (*it == w){
            windows_.erase(it);
            return;
        }
    }
    LOG_ERROR("X11::EventLoop::unregisterWindow: bug");
}

/*///////////////// Window ////////////////////*/

Window::Window(Display& display, IPlugin& plugin)
    : display_(&display), plugin_(&plugin) {
    // cache for buggy plugins!
    canResize_ = plugin_->canResize();
}

Window::~Window(){
    doClose();
}

void Window::open(){
    EventLoop::instance().callAsync([](void *x){
        static_cast<Window *>(x)->doOpen();
    }, this);
}

void Window::doOpen(){
    LOG_DEBUG("X11: open");
    if (window_){
        // just bring to foreground
        LOG_DEBUG("X11: restore");
    #if 0
        XRaiseWindow(display_, window_);
    #else
        savePosition();
        XUnmapWindow(display_, window_);
        XMapWindow(display_, window_);
        XMoveWindow(display_, window_, rect_.x, rect_.y);
    #endif
        XFlush(display_);
        return;
    }

    // Create window with dummy (non-empty!) size.
    // Already set 'window_' in the beginning because openEditor()
    // might implicitly call resize()!
    int s = DefaultScreen(display_);
    window_ = XCreateSimpleWindow(display_, RootWindow(display_, s),
                0, 0, 300, 300, 1,
                BlackPixel(display_, s), WhitePixel(display_, s));
    // receive configure events
    XSelectInput(display_, window_, StructureNotifyMask);
    // Intercept request to delete window when being closed
    XSetWMProtocols(display_, window_, &wmDelete, 1);
    // set window class hint
    XClassHint *ch = XAllocClassHint();
    if (ch){
        ch->res_name = (char *)"VST Editor";
        ch->res_class = (char *)"VST Editor Window";
        XSetClassHint(display_, window_, ch);
        XFree(ch);
    }
    // set window title
    auto title = plugin_->info().name.c_str();
    XStoreName(display_, window_, title);
    XSetIconName(display_, window_, title);
    LOG_DEBUG("X11: created Window " << window_);

    // set window coordinates
    bool didOpen = false;
    if (rect_.valid()){
        LOG_DEBUG("restore window");
        // just restore from cached rect
    } else {
        // get window dimensions from plugin
        Rect r;
        if (!plugin_->getEditorRect(r)){
            // HACK for plugins which don't report the window size
            // without the editor being opened
            LOG_DEBUG("couldn't get editor rect!");
            plugin_->openEditor(getHandle());
            plugin_->getEditorRect(r);
            didOpen = true;
        }
        LOG_DEBUG("editor size: " << r.w << " * " << r.h);
        // only set size!
        rect_.w = r.w;
        rect_.h = r.h;
    }

    // disable resizing
    if (!canResize_) {
        LOG_DEBUG("X11: disable resizing");
        setFixedSize(rect_.w, rect_.h);
    } else {
        LOG_DEBUG("X11: enable resizing");
    }

    // resize the window
    XMapWindow(display_, window_);
    XMoveResizeWindow(display_, window_, rect_.x, rect_.y, rect_.x, rect_.y);
    XFlush(display_);

    // open VST editor
    if (!didOpen){
        LOG_DEBUG("open editor");
        plugin_->openEditor(getHandle());
    }

    LOG_DEBUG("X11: register Window");
    EventLoop::instance().registerWindow(this);
}

void Window::close(){
    EventLoop::instance().callAsync([](void *x){
        static_cast<Window *>(x)->doClose();
    }, this);
}

void Window::doClose(){
    if (window_){
        LOG_DEBUG("X11: close");
        savePosition();

        LOG_DEBUG("X11: unregister Window");
        EventLoop::instance().unregisterWindow(this);

        LOG_DEBUG("close editor");
        plugin_->closeEditor();

        XDestroyWindow(display_, window_);
        window_ = 0;
        LOG_DEBUG("X11: destroyed Window");
    }
}

void Window::setFixedSize(int w, int h){
    XSizeHints *hints = XAllocSizeHints();
    if (hints){
        hints->flags = PMinSize | PMaxSize;
        hints->max_width = hints->min_width = w;
        hints->min_height = hints->max_height = h;
        XSetWMNormalHints(display_, window_, hints);
        XFree(hints);
    }
}

void Window::savePosition(){
    auto root = EventLoop::instance().getRoot();
    int x, y;
    ::Window child;
    XWindowAttributes xwa;
    XTranslateCoordinates(display_, window_, root, 0, 0, &x, &y, &child);
    XGetWindowAttributes(display_, window_, &xwa);
    // somehow it's 2 pixels off, probably because of the border
    rect_.x = x - xwa.x + 2;
    rect_.y = y - xwa.y + 2;
    LOG_DEBUG("X11: save position " << rect_.x << ", " << rect_.y);
}

void Window::setPos(int x, int y){
    EventLoop::instance().callAsync([](void *user){
        auto cmd = static_cast<Command *>(user);
        auto window = cmd->owner->window_;
        auto display = cmd->owner->display_;
        auto& r = cmd->owner->rect_;
        // cache!
        r.x = cmd->x;
        r.y = cmd->y;

        if (window){
            XMoveWindow(display, window, r.x, r.y);
            XFlush(display);
        }

        delete cmd;
    }, new Command { this, x, y });
}

void Window::setSize(int w, int h){
    LOG_DEBUG("setSize: " << w << ", " << h);
    EventLoop::instance().callAsync([](void *user){
        auto cmd = static_cast<Command *>(user);
        auto owner = cmd->owner;
        if (owner->canResize_){
            auto window = owner->window_;
            auto display = owner->display_;
            auto& r = cmd->owner->rect_;
            // cache!
            r.w = cmd->x;
            r.h = cmd->y;

            if (window){
                XResizeWindow(display, window, r.w, r.h);
                XFlush(display);
            }
        }

        delete cmd;
    }, new Command { this, w, h });
}

void Window::resize(int w, int h){
    LOG_DEBUG("resized by plugin: " << w << ", " << h);
    // should only be called if the window is open
    if (window_){
        if (!canResize_){
            setFixedSize(w, h);
        }
        XResizeWindow(display_, window_, w, h);
        XFlush(display_);

        // cache!
        rect_.w = w;
        rect_.h = h;
    }
}

void Window::onClose(){
    doClose();
}

void Window::onUpdate(){
    plugin_->updateEditor();
}

void Window::onConfigure(int x, int y, int width, int height){
    LOG_DEBUG("onConfigure: x: "<< x << ", y: " << y
              << ", w: " << width << ", h: " << height);
    if (canResize_ && (rect_.w != width || rect_.h != height)){
        LOG_DEBUG("size changed");
        plugin_->resizeEditor(width, height);
        rect_.w = width;
        rect_.h = height;
    }
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
