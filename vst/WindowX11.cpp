#include "WindowX11.h"

#include "PluginDesc.h"
#include "MiscUtils.h"

#include <algorithm>
#include <cstring>
#include <chrono>

#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>

namespace vst {

namespace UIThread {

static thread_local bool gCurrentThreadUI = false;

// NB: this check must *not* implicitly create the event loop!
bool isCurrentThread() {
    return gCurrentThreadUI;
}

void setup() {
    // HACK: this tells the EventLoop constructor that it shouldn't create an UI thread
    gCurrentThreadUI = true;
    X11::EventLoop::instance();
}

void run() {
    X11::EventLoop::instance().run();
}

void quit() {
    X11::EventLoop::instance().quit();
}

bool available() {
    return X11::EventLoop::instance().available();
}

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
        LOG_WARNING("X11: XInitThreads failed!");
    }
    display_ = XOpenDisplay(nullptr);
    if (!display_){
        LOG_ERROR("X11: couldn't open display! No X11 server?");
        return;
    }
    displayfd_ = ::XConnectionNumber(display_);
    // install error handler, so our program won't die on a bad X11 request
    XSetErrorHandler([](Display *d, XErrorEvent *e){
        char buf[256];
        buf[0] = 0;
        XGetErrorText(d, e->error_code, buf, sizeof(buf));
        LOG_ERROR("X11: Error: " << buf);
        return 0;
    });

    eventfd_ = eventfd(0, 0);
    if (eventfd_ < 0){
        throw Error("X11: couldn't create eventfd");
    }
#if 0
    // root_ = DefaultRootWindow(display_);
#else
    // for some reason, the "real" root window doesn't receive
    // client messages, so I have to create a dummy window instead...
    root_ = XCreateSimpleWindow(display_, DefaultRootWindow(display_),
                0, 0, 1, 1, 1, 0, 0);
#endif
    if (!root_) {
        throw Error("X11: couldn't create root window!");
    }
    LOG_DEBUG("X11: created root window: " << root_);
    wmProtocols = XInternAtom(display_, "WM_PROTOCOLS", 0);
    wmDelete = XInternAtom(display_, "WM_DELETE_WINDOW", 0);

    running_.store(true);
    if (UIThread::isCurrentThread()) {
        initUIThread();
    } else {
        // start thread
        LOG_DEBUG("X11: start UI thread");
        thread_ = std::thread([&](){
            initUIThread();
            run();
        });
    }
}

EventLoop::~EventLoop(){
    if (thread_.joinable()){
        // notify thread and wait
        running_.store(false);
        notify();
        thread_.join();
        LOG_DEBUG("X11: terminated UI thread");
    }
    if (display_){
    #if 1
        if (root_) {
            // destroy dummy window
            XDestroyWindow(display_, root_);
        }
    #endif
        XCloseDisplay(display_);
    }
    if (eventfd_ >= 0){
        close(eventfd_);
    }
}

void EventLoop::run() {
    assert(UIThread::isCurrentThread());
    LOG_DEBUG("X11: start event loop");
    while (running_.load()) {
        auto wait = updateTimers();

        pollFileDescriptors(wait);

        pollX11Events();

        handleCommands();
    }
    LOG_DEBUG("X11: quit event loop");
}

void EventLoop::quit() {
    running_.store(false);
    notify();
}

void EventLoop::initUIThread() {
    setThreadPriority(Priority::Low);
    UIThread::gCurrentThreadUI = true;
}

void EventLoop::pushCommand(UIThread::Callback cb, void *user){
    std::lock_guard lock(commandMutex_);
    commands_.push_back({ cb, user });
    notify();
}

int EventLoop::updateTimers() {
    TimePoint now = std::chrono::time_point_cast<Milliseconds>(Clock::now());

    while (!timerQueue_.empty() && timerQueue_.front().deadline <= now) {
        // NB: make copy, in case the timer function adds/removes timers!
        Timer timer = timerQueue_.front();
        std::pop_heap(timerQueue_.begin(), timerQueue_.end(), Timer::Compare{});
        timerQueue_.pop_back();
        // NB: reschedule *before* calling the function, so the timer can
        // be found and removed from within the timer function!
        timer.sequence = timerSequence_++;
        if (timer.deadline == TimePoint{}) {
            // first update
            timer.deadline = now + Milliseconds(timer.interval);
        } else {
            timer.deadline += Milliseconds(timer.interval);
        }
        timerQueue_.push_back(timer);
        std::push_heap(timerQueue_.begin(), timerQueue_.end(), Timer::Compare{});
        // finally call the function
        timer.cb(timer.obj);
    }

    if (!timerQueue_.empty()) {
        // wait for next due timer
        auto next = timerQueue_.front().deadline;
        now = std::chrono::time_point_cast<Milliseconds>(Clock::now());
        if (next > now) {
            auto wait = (next - now).count();
            // LOG_DEBUG("X11: wait for " << wait << " ms");
            return wait;
        } else {
            return 0; // don't wait
        }
    } else {
        return -1; // sleep
    }
}

void EventLoop::pollFileDescriptors(int timeout){
    // NB: we copy the fd array to prevent it from being modified
    // from within event handlers!
    int count = eventHandlers_.size();
    // allocate extra fds for eventfd_ and displayfd_
    auto fds = (pollfd *)alloca(sizeof(pollfd) * (count + 2));
    auto it = eventHandlers_.begin();
    for (int i = 0; i < count; ++i, ++it){
        fds[i].fd = it->first;
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }
    auto event_index = count;
    fds[event_index].fd = eventfd_;
    fds[event_index].events = POLLIN;
    fds[event_index].revents = 0;
    auto display_index = count + 1;
    fds[display_index].fd = displayfd_;
    fds[display_index].events = POLLIN;
    fds[display_index].revents = 0;

    if (timeout < 0) {
        LOG_DEBUG("X11: waiting...");
    }

    auto result = poll(fds, count + 1, timeout);
    if (result > 0) {
        // check registered event handler fds
        for (int i = 0; i < count; ++i) {
            if (auto revents = fds[i].revents; revents != 0){
                auto fd = fds[i].fd;
                // check if handler still exists!
                auto it = eventHandlers_.find(fd);
                if (it != eventHandlers_.end()){
                    auto& handler = it->second;
                    if (revents & POLLIN){
                        LOG_DEBUG("X11: fd " << fd << " became ready!");
                        handler.cb(fd, handler.obj);
                    } else {
                        LOG_ERROR("X11: eventfd " << fd
                                  << ": error - removing event handler");
                        eventHandlers_.erase(fd);
                    }
                }
            }
        }
        // check our eventfd
        if (auto revents = fds[event_index].revents; revents != 0) {
            if (revents & POLLIN){
                uint64_t data;
                auto n = read(eventfd_, &data, sizeof(data));
                if (n < 0){
                    LOG_ERROR("X11: couldn't read eventfd: " << strerror(errno));
                } else if (n != sizeof(data)){
                    LOG_ERROR("X11: read wrong number of bytes from eventfd");
                }
            } else {
                LOG_ERROR("X11: eventfd poll error");
                running_.store(false);
            }
        }
        // check displayfd
        if (auto revents = fds[display_index].revents; revents != 0) {
            if (revents & POLLIN){
                // LOG_DEBUG("X11: display ready");
            } else {
                LOG_ERROR("X11: display poll error");
                running_.store(false);
            }
        }
    }
}

void EventLoop::pollX11Events(){
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

void EventLoop::handleCommands() {
    std::unique_lock lock(commandMutex_);
    while (!commands_.empty()) {
        // first swap pending commands
        std::vector<Command> commands;
        std::swap(commands, commands_);
        // execute callbacks without the mutex locked to avoid deadlocks
        // (although it is unlikely that a command will push another command in turn).
        lock.unlock();
        for (auto& cmd : commands){
            cmd.cb(cmd.obj);
        }
        lock.lock();
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
        std::lock_guard lock(syncMutex_); // prevent concurrent calls
        pushCommand([](void *x){
            static_cast<EventLoop *>(x)->event_.set();
        }, this);
        LOG_DEBUG("X11: wait for sync event...");
        event_.wait();
        LOG_DEBUG("X11: synchronized!");
    }
    return true;
}

bool EventLoop::callSync(UIThread::Callback cb, void *user){
    if (UIThread::isCurrentThread()){
        cb(user);
    } else {
        std::lock_guard lock(syncMutex_); // prevent concurrent calls!
        auto cmd = [&](){
            cb(user);
            event_.set();
        };
        pushCommand([](void *x){
            // call the lambda
            (*static_cast<decltype (cmd) *>(x))();
        }, &cmd);
        LOG_DEBUG("X11: wait for sync event...");
        event_.wait();
        LOG_DEBUG("X11: synchronized");
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

void EventLoop::registerWindow(Window *w) {
    assert(UIThread::isCurrentThread());
    for (auto& it : windows_){
        if (it == w){
            LOG_ERROR("X11::EventLoop::registerWindow: bug");
            return;
        }
    }
    windows_.push_back(w);
    doRegisterTimer(updateIntervalMillis, [](void *x){
        static_cast<Window *>(x)->onUpdate();
    }, w);
}

void EventLoop::unregisterWindow(Window *w) {
    assert(UIThread::isCurrentThread());
    auto it = std::find(windows_.begin(), windows_.end(), w);
    if (it != windows_.end()) {
        doUnregisterTimer(w);
        windows_.erase(it);
        return;
    }
    LOG_ERROR("X11::EventLoop::unregisterWindow: bug");
}

void EventLoop::registerEventHandler(int fd, EventHandlerCallback cb, void *obj){
    bool ok = UIThread::isCurrentThread();
    assert(ok);
    if (ok) {
        eventHandlers_[fd] = EventHandler { obj, cb };
    } else {
        LOG_ERROR("X11: registerEventHandler() called on wrong thread!");
    }
}

void EventLoop::unregisterEventHandler(void *obj){
    bool ok = UIThread::isCurrentThread();
    assert(ok);
    if (ok) {
        int count = 0;
        for (auto it = eventHandlers_.begin(); it != eventHandlers_.end(); ){
            if (it->second.obj == obj){
                it = eventHandlers_.erase(it);
                count++;
            } else {
                ++it;
            }
        }
        LOG_DEBUG("X11: unregistered " << count << " event handlers");
    } else {
        LOG_ERROR("X11: unregisterEventHandler() called on wrong thread!");
    }
}

void EventLoop::registerTimer(int64_t ms, TimerCallback cb, void *obj){
    bool ok = UIThread::isCurrentThread();
    assert(ok);
    if (ok) {
        doRegisterTimer(ms, cb, obj);
    } else {
        LOG_ERROR("X11: registerTimer() called on wrong thread!");
    }
}

void EventLoop::doRegisterTimer(int64_t ms, TimerCallback cb, void *obj){
    timerQueue_.push_back(Timer{cb, obj, ms, timerSequence_++});
    std::push_heap(timerQueue_.begin(), timerQueue_.end(), Timer::Compare{});
    LOG_DEBUG("X11: registered timer (" << obj << ")");
}

void EventLoop::unregisterTimer(void *obj) {
    bool ok = UIThread::isCurrentThread();
    assert(ok);
    if (ok) {
        doUnregisterTimer(obj);
    } else {
        LOG_ERROR("X11: unregisterTimer() called on wrong thread!");
    }
}

void EventLoop::doUnregisterTimer(void *obj){
    int count = 0;
    for (auto it = timerQueue_.begin(); it != timerQueue_.end();) {
        if (it->obj == obj) {
            it = timerQueue_.erase(it);
            count++;
        } else {
            ++it;
        }
    }
    std::make_heap(timerQueue_.begin(), timerQueue_.end(), Timer::Compare{});
    LOG_DEBUG("X11: unregistered " << count << " timers (" << obj << ")");
}

void EventLoop::startPolling() {
    if (std::find_if(timerQueue_.begin(), timerQueue_.end(),
                     [&](auto& x) { return x.obj == this; }) != timerQueue_.end()) {
        LOG_ERROR("EventLoop: poll function timer already installed!");
        return;
    }
    doRegisterTimer(updateIntervalMillis, [](void *x) {
        static_cast<EventLoop *>(x)->doPoll();
    }, this);
}

void EventLoop::stopPolling() {
    doUnregisterTimer(this);
}

/*///////////////// Window ////////////////////*/

Window::Window(Display& display, IPlugin& plugin)
    : display_(&display), plugin_(&plugin) {}

Window::~Window(){
    doClose();
}

bool Window::canResize() const {
    return plugin_->info().editorResizable();
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
    if (rect_.valid()) {
        LOG_DEBUG("X11: restore editor rect");
        // just restore from cached rect
    } else {
        // get window dimensions from plugin
        Rect r;
        if (!plugin_->getEditorRect(r)){
            // HACK for plugins which don't report the window size
            // without the editor being opened
            LOG_DEBUG("X11: couldn't get editor rect!");
            // Flush before opening the editor! Otherwise, some plugins
            // (e.g. LSP plugins) would only show a blank window.
            XFlush(display_);
            plugin_->openEditor(getHandle());
            plugin_->getEditorRect(r);
            didOpen = true;
        }
        LOG_DEBUG("X11: editor size: " << r.w << " * " << r.h);
        // only set size!
        rect_.w = r.w;
        rect_.h = r.h;
    }

    // disable resizing
    if (!canResize()) {
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
        LOG_DEBUG("X11: open editor");
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

        LOG_DEBUG("X11: close editor");
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
    LOG_DEBUG("X11: setSize: " << w << ", " << h);
    EventLoop::instance().callAsync([](void *user){
        auto cmd = static_cast<Command *>(user);
        auto owner = cmd->owner;
        if (owner->canResize()){
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
    LOG_DEBUG("X11: resized by plugin: " << w << ", " << h);
    // should only be called if the window is open
    if (window_){
        if (!canResize()){
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
    LOG_DEBUG("X11: onConfigure: x: "<< x << ", y: " << y
              << ", w: " << width << ", h: " << height);
    if (canResize() && (rect_.w != width || rect_.h != height)){
        LOG_DEBUG("X11: size changed");
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
