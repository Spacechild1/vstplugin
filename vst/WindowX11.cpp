#include "WindowX11.h"
#include "Utility.h"

#include <cstring>
#include <cassert>
#include <chrono>

#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>

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
    eventfd_ = eventfd(0, 0);
    if (eventfd_ < 0){
        throw Error("X11: couldn't create eventfd");
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
    // run thread
    running_.store(true);
    thread_ = std::thread(&EventLoop::run, this);
    LOG_DEBUG("X11: UI thread ready");
}

EventLoop::~EventLoop(){
    if (thread_.joinable()){
        // notify thread and wait
        running_.store(false);
        notify();
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
    if (eventfd_ >= 0){
        close(eventfd_);
    }
}

void EventLoop::pushCommand(UIThread::Callback cb, void *user){
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back({ cb, user });
    notify();
}

void EventLoop::run(){
    setThreadPriority(Priority::Low);

    LOG_DEBUG("X11: start event loop");

    auto last = std::chrono::high_resolution_clock::now();

    while (running_.load()){
        pollFds();

        pollEvents();

        auto now = std::chrono::high_resolution_clock::now();
        auto delta = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;

        // handle timers
        // lock because timers can be added from another thread!
        std::unique_lock<std::mutex> lock(mutex_);
        for (auto& timer : timerList_){
            timer.elapsed += delta;
            while (timer.elapsed > (double)timer.interval){
                timer.cb(timer.obj);
                timer.elapsed -= (double)timer.interval;
            }
        }
        // handle commands
        // don't execute callbacks while holding the lock to
        // avoid a deadlock (e.g. PluginBridge calls addPollFunction())
        std::vector<Command> commands = std::move(commands_);
        commands_.clear();
        lock.unlock();
        for (auto& cmd : commands){
            cmd.cb(cmd.obj);
        }
    }
}

void EventLoop::pollFds(){
    std::unique_lock<std::mutex> lock(mutex_);

    int count = eventHandlers_.size();
    // allocate extra fd for eventfd_
    auto fds = (pollfd *)alloca(sizeof(pollfd) * (count + 1));
    auto it = eventHandlers_.begin();
    for (int i = 0; i < count; ++i, ++it){
        fds[i].fd = it->first;
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }
    fds[count].fd = eventfd_;
    fds[count].events = POLLIN;
    fds[count].revents = 0;

    // unlock before poll!
    lock.unlock();
    auto result = poll(fds, count + 1, sleepGrain);
    if (result > 0){
        // lock again!
        lock.lock();
        // check registered event handler fds
        for (int i = 0; i < count; ++i){
            auto revents = fds[i].revents;
            if (revents != 0){
                auto fd = fds[i].fd;
                // check if handler still exists!
                auto it = eventHandlers_.find(fd);
                if (it != eventHandlers_.end()){
                    auto& handler = it->second;
                    if (revents & POLLIN){
                        LOG_DEBUG("fd " << fd << " became ready!");
                        handler.cb(fd, handler.obj);
                    } else {
                        LOG_ERROR("eventfd " << fd << ": error - removing event handler");
                        eventHandlers_.erase(fd);
                    }
                }
            }
        }
        // check our eventfd
        auto revents = fds[count].revents;
        if (revents != 0){
            if (revents & POLLIN){
                uint64_t data;
                auto n = read(eventfd_, &data, sizeof(data));
                if (n < 0){
                    LOG_ERROR("X11: couldn't read eventfd: " << strerror(errno));
                } else if (n != sizeof(data)){
                    LOG_ERROR("X11: read wrong number of bytes from eventfd");
                }
            } else {
                LOG_ERROR("X11: eventfd error");
                running_.store(false);
            }
        }
    }
}

void EventLoop::pollEvents(){
    while (XPending(display_)){
        XEvent event;
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

void EventLoop::notify(){
    uint64_t i = 1;
    if (write(eventfd_, &i, sizeof(i)) < 0){
        LOG_ERROR("X11: couldn't write to eventfd: " << strerror(errno));
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
    if (!UIThread::isCurrentThread()){
        std::lock_guard<std::mutex> lock(syncMutex_); // prevent concurrent calls
        pushCommand([](void *x){
            static_cast<EventLoop *>(x)->event_.set();
        }, this);
        LOG_DEBUG("waiting...");
        event_.wait();
        LOG_DEBUG("done");
    }
    return true;
}

bool EventLoop::callSync(UIThread::Callback cb, void *user){
    if (UIThread::isCurrentThread()){
        cb(user);
    } else {
        std::lock_guard<std::mutex> lock(syncMutex_); // prevent concurrent calls
        auto cmd = [&](){
            cb(user);
            event_.set();
        };
        pushCommand([](void *x){
            // call the lambda
            (*static_cast<decltype (cmd) *>(x))();
        }, &cmd);
        LOG_DEBUG("waiting...");
        event_.wait();
        LOG_DEBUG("done");
    }
    return true;
}

bool EventLoop::callAsync(UIThread::Callback cb, void *user){
    if (UIThread::isCurrentThread()){
        cb(user);
    } else {
        pushCommand(cb, user);
    }
    return true;
}

UIThread::Handle EventLoop::addPollFunction(UIThread::PollFunction fn,
                                            void *context){
    std::lock_guard<std::mutex> lock(mutex_);
    auto handle = nextPollFunctionHandle_++;
    pollFunctions_.emplace(handle, context);
    doRegisterTimer(updateInterval, fn, context);
    return handle;
}

void EventLoop::removePollFunction(UIThread::Handle handle){
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pollFunctions_.find(handle);
    if (it != pollFunctions_.end()){
        // we assume that we only ever register a single
        // poll function for a given context
        doUnregisterTimer(it->second);
        pollFunctions_.erase(it);
    } else {
        LOG_ERROR("X11: couldn't remove poll function");
    }
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
    doRegisterTimer(updateInterval, [](void *x){
        static_cast<Window *>(x)->onUpdate();
    }, w);
}

void EventLoop::unregisterWindow(Window *w){
    for (auto it = windows_.begin(); it != windows_.end(); ++it){
        if (*it == w){
            doUnregisterTimer(w);
            windows_.erase(it);
            return;
        }
    }
    LOG_ERROR("X11::EventLoop::unregisterWindow: bug");
}

void EventLoop::registerEventHandler(int fd, EventHandlerCallback cb, void *obj){
#if 1
    assert(UIThread::isCurrentThread());
#else
    std::lock_guard<std::mutex> lock(mutex_);
#endif
    eventHandlers_[fd] = EventHandler { obj, cb };
}

void EventLoop::unregisterEventHandler(void *obj){
#if 1
    assert(UIThread::isCurrentThread());
#else
    std::lock_guard<std::mutex> lock(mutex_);
#endif
    int count = 0;
    for (auto it = eventHandlers_.begin(); it != eventHandlers_.end(); ){
        if (it->second.obj == obj){
            it = eventHandlers_.erase(it);
            count++;
        } else {
            ++it;
        }
    }
    LOG_DEBUG("unregistered " << count << " handlers");
}

void EventLoop::registerTimer(int64_t ms, TimerCallback cb, void *obj){
#if 1
    assert(UIThread::isCurrentThread());
#else
    std::lock_guard<std::mutex> lock(mutex_);
#endif
    doRegisterTimer(ms, cb, obj);
}

void EventLoop::doRegisterTimer(int64_t ms, TimerCallback cb, void *obj){
    timerList_.push_back({ obj, cb, ms, 0.0 });
}

void EventLoop::unregisterTimer(void *obj){
#if 1
    assert(UIThread::isCurrentThread());
#else
    std::lock_guard<std::mutex> lock(mutex_);
#endif
    doUnregisterTimer(obj);
}

void EventLoop::doUnregisterTimer(void *obj){
    int count = 0;
    for (auto it = timerList_.begin(); it != timerList_.end(); ){
        if (it->obj == obj){
            it = timerList_.erase(it);
            count++;
        } else {
            ++it;
        }
    }
    LOG_DEBUG("unregistered " << count << " timers");
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
    if (canResize_ && rect_.valid()){
        LOG_DEBUG("X11: restore editor size");
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
