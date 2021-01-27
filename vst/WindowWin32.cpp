#include "WindowWin32.h"
#include "Utility.h"

#include <cstring>
#include <process.h>

#define VST_EDITOR_CLASS_NAME L"VST Plugin Editor Class"

namespace vst {

namespace UIThread {

// fake event loop
Event gQuitEvent_;

void setup(){
    Win32::EventLoop::instance();
}

void run(){
    gQuitEvent_.wait();
}

void quit(){
    gQuitEvent_.set();
}

bool isCurrentThread() {
    return Win32::EventLoop::instance().checkThread();
}

bool available() { return true; }

void poll(){}

bool sync(){
    return Win32::EventLoop::instance().sync();
}

bool callSync(Callback cb, void *user){
    return Win32::EventLoop::instance().callSync(cb, user);
}

bool callAsync(Callback cb, void *user){
    return Win32::EventLoop::instance().callAsync(cb, user);
}

int32_t addPollFunction(PollFunction fn, void *context){
    return Win32::EventLoop::instance().addPollFunction(fn, context);
}

void removePollFunction(int32_t handle){
    return Win32::EventLoop::instance().removePollFunction(handle);
}

} // UIThread

namespace Win32 {

/*/////////////////// EventLoop //////////////////////*/

EventLoop& EventLoop::instance(){
    static EventLoop thread;
    return thread;
}

DWORD EventLoop::run(void *user){
    setThreadPriority(Priority::Low);

    auto obj = (EventLoop *)user;
    // force message queue creation
    MSG msg;
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    obj->notify();
    LOG_DEBUG("start message loop");

    // setup timer
    auto timer = SetTimer(0, 0, EventLoop::updateInterval, NULL);

    DWORD ret;
    while ((ret = GetMessage(&msg, NULL, 0, 0)) != 0){
        if (ret < 0){
            LOG_ERROR("GetMessage: error");
            break;
        }
        auto type = msg.message;
        if (type == WM_CALL){
            LOG_DEBUG("WM_CALL");
            auto cb = (UIThread::Callback)msg.wParam;
            auto data = (void *)msg.lParam;
            cb(data);
        } else if (type == WM_SYNC){
            LOG_DEBUG("WM_SYNC");
            obj->notify();
        } else if ((type == WM_TIMER) && (msg.hwnd == NULL)
                   && (msg.wParam == timer)) {
            // call poll functions
            std::lock_guard<std::mutex> lock(obj->pollFunctionMutex_);
            for (auto& it : obj->pollFunctions_){
                it.second();
            }
        } else {
            // LOG_DEBUG("dispatch message " << msg.message);
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    LOG_DEBUG("quit message loop");

    KillTimer(NULL, timer);

    return 0;
}

EventLoop::EventLoop(){
    // setup window class
    WNDCLASSEXW wcex;
    memset(&wcex, 0, sizeof(WNDCLASSEXW));
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.lpfnWndProc = Window::procedure;
    wcex.lpszClassName =  VST_EDITOR_CLASS_NAME;
    wchar_t exeFileName[MAX_PATH];
    GetModuleFileNameW(NULL, exeFileName, MAX_PATH);
    wcex.hIcon = ExtractIconW(NULL, exeFileName, 0);
    if (!RegisterClassExW(&wcex)){
        LOG_WARNING("couldn't register window class!");
    } else {
        LOG_DEBUG("registered window class!");
    }
    // run thread
    thread_ = CreateThread(NULL, 0, run, this, 0, &threadID_);
    if (!thread_){
        throw Error("couldn't create UI thread!");
    };
    // wait for thread to create message queue
    event_.wait();
    LOG_DEBUG("message queue created");
}

EventLoop::~EventLoop(){
    LOG_DEBUG("EventLoop: about to quit");
    if (thread_){
        if (postMessage(WM_QUIT)){
            WaitForSingleObject(thread_, INFINITE);
            LOG_DEBUG("joined thread");
        } else {
            LOG_ERROR("couldn't post quit message!");
        }
        CloseHandle(thread_);
    }
}

bool EventLoop::checkThread() {
    return GetCurrentThreadId() == threadID_;
}

bool EventLoop::postMessage(UINT msg, void *data1, void *data2){
    return PostThreadMessage(threadID_, msg, (WPARAM)data1, (LPARAM)data2);
}

bool EventLoop::callAsync(UIThread::Callback cb, void *user){
    if (UIThread::isCurrentThread()){
        cb(user);
        return true;
    } else {
        return postMessage(WM_CALL, (void *)cb, (void *)user);
    }
}

bool EventLoop::callSync(UIThread::Callback cb, void *user){
    if (UIThread::isCurrentThread()){
        cb(user);
        return true;
    } else {
        std::lock_guard<std::mutex> lock(mutex_); // prevent concurrent calls
        if (postMessage(WM_CALL, (void *)cb, (void *)user)
                && postMessage(WM_SYNC))
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

bool EventLoop::sync(){
    if (UIThread::isCurrentThread()){
        return true;
    } else {
        std::lock_guard<std::mutex> lock(mutex_); // prevent concurrent calls
        if (postMessage(WM_SYNC)){
            LOG_DEBUG("waiting...");
            event_.wait();
            LOG_DEBUG("done");
            return true;
        } else {
            return false;
        }
    }
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

void EventLoop::notify(){
    event_.set();
}

/*///////////////////////// Window ///////////////////////////*/

Window::Window(IPlugin& plugin)
    : plugin_(&plugin) {
    // cache for buggy plugins!
    canResize_ = plugin_->canResize();
}

Window::~Window(){
    doClose();
}

LRESULT WINAPI Window::procedure(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam){
    auto window = (Window *)GetWindowLongPtr(hWnd, GWLP_USERDATA);

    switch (Msg){
    case WM_CLOSE: // intercept close event!
    {
        if (window){
            window->doClose();
        } else {
            LOG_ERROR("bug GetWindowLongPtr");
        }
        return true;
    }
    case WM_SIZING:
    {
        LOG_DEBUG("WM_SIZING");
        if (window){
            window->onSizing(*(RECT *)lParam);
        } else {
            LOG_ERROR("bug GetWindowLongPtr");
        }
        return true;
    }
    case WM_SIZE:
    {
        LOG_DEBUG("WM_SIZE");
        if (wParam == SIZE_MAXIMIZED || wParam == SIZE_RESTORED){
            if (window){
                window->onSize(LOWORD(lParam), HIWORD(lParam));
            } else {
                LOG_ERROR("bug GetWindowLongPtr");
            }
        }
        return true;
    }
    default:
        return DefWindowProcW(hWnd, Msg, wParam, lParam);
    }
}

void CALLBACK Window::updateEditor(HWND hwnd, UINT msg, UINT_PTR id, DWORD time){
    auto window = (Window *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (window){
        window->plugin_->updateEditor();
        // LOG_DEBUG("update editor");
    } else {
        LOG_ERROR("bug GetWindowLongPtr");
    }
}

void Window::open(){
    EventLoop::instance().callAsync([](void *x){
        static_cast<Window *>(x)->doOpen();
    }, this);
}

void Window::doOpen(){
    if (hwnd_){
        // just show the window
        ShowWindow(hwnd_, SW_MINIMIZE);
        ShowWindow(hwnd_, SW_RESTORE);
        BringWindowToTop(hwnd_);
        return;
    }

    // no maximize box if plugin view can't be resized
    DWORD dwStyle = canResize_ ?
                WS_OVERLAPPEDWINDOW :
                (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
    // already set 'hwnd_' in the beginning because openEditor()
    // might implicitly call setSize()!
    hwnd_ = CreateWindowW(
          VST_EDITOR_CLASS_NAME, L"Untitled",
          dwStyle, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
          NULL, NULL, NULL, NULL
    );
    LOG_DEBUG("Win32: created Window");
    // set window title
    SetWindowTextW(hwnd_, widen(plugin_->info().name).c_str());
    // set user data
    SetWindowLongPtr(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    // set window coordinates
    bool didOpen = false;
    if (rect_.valid()){
        LOG_DEBUG("restore window");
        // restore from cached rect
        // ignore resize request if we can't resize!
        if (resizeRequest_ && canResize_){
            // meet resize request
            rect_.w = rw_;
            rect_.h = rh_;
            adjustRect();
            resizeRequest_ = false;
        }
        MoveWindow(hwnd_, rect_.x, rect_.y, rect_.w, rect_.h, TRUE);
        // NOTE: restoring the size doesn't work if openEditor()
        // calls setSize() in turn! I've tried various workarounds,
        // like setting a flag and bashing the size in setSize(),
        // but they all cause weirdness...
    } else {
        // get window dimensions from plugin
        Rect r;
        if (!plugin_->getEditorRect(r)){
            // HACK for plugins which don't report the window size
            // without the editor being opened
            LOG_DEBUG("couldn't get editor rect!");
            plugin_->openEditor(hwnd_);
            plugin_->getEditorRect(r);
            didOpen = true;
        }
        LOG_DEBUG("editor size: " << r.w << " * " << r.h);
        // set and adjust size
        rect_.w = r.w;
        rect_.h = r.h;
        adjustRect();
        MoveWindow(hwnd_, rect_.x, rect_.y, rect_.w, rect_.h, TRUE);
        resizeRequest_ = false; // just to be sure
    }

    // open VST editor
    if (!didOpen){
        plugin_->openEditor(hwnd_);
    }

    // show window
#if 0
    SetForegroundWindow(hwnd_);
    ShowWindow(hwnd_, SW_SHOW);
    BringWindowToTop(hwnd_);
#else
    ShowWindow(hwnd_, SW_MINIMIZE);
    ShowWindow(hwnd_, SW_RESTORE);
#endif

    SetTimer(hwnd_, timerID, EventLoop::updateInterval, &updateEditor);

    LOG_DEBUG("Win32: setup Window done");
}

void Window::close(){
    EventLoop::instance().callAsync([](void *x){
        static_cast<Window *>(x)->doClose();
    }, this);
}

void Window::doClose(){
    if (hwnd_){
        RECT rc;
        if (GetWindowRect(hwnd_, &rc)){
            // cache position and size
            rect_.x = rc.left;
            rect_.y = rc.top;
            rect_.w = rc.right - rc.left;
            rect_.h = rc.bottom - rc.top;
            resizeRequest_ = false; // to be sure
        }

        KillTimer(hwnd_, timerID);

        plugin_->closeEditor();

        DestroyWindow(hwnd_);
        hwnd_ = NULL;
        LOG_DEBUG("Win32: destroyed Window");
    }
}

void Window::setPos(int x, int y){
    EventLoop::instance().callAsync([](void *user){
        auto cmd = static_cast<Command *>(user);
        auto owner = cmd->owner;
        auto& r = owner->rect_;
        // update position
        r.x = cmd->x;
        r.y = cmd->y;
        if (owner->hwnd_){
            MoveWindow(owner->hwnd_, r.x, r.y, r.w, r.h, TRUE);
        }
        delete cmd;
    }, new Command { this, x, y });
}

// always the client rect size!
void Window::setSize(int w, int h){
    LOG_DEBUG("setSize: " << w << ", " << h);
    EventLoop::instance().callAsync([](void *user){
        auto cmd = static_cast<Command *>(user);
        auto owner = cmd->owner;
        if (owner->hwnd_){
            // update and adjust size
            auto& r = owner->rect_;
            r.w = cmd->x;
            r.h = cmd->y;
            owner->adjustRect();
            MoveWindow(owner->hwnd_, r.x, r.y, r.w, r.h, TRUE);
            owner->resizeRequest_ = false; // to be sure
        } else {
            // request resize
            owner->rw_ = cmd->x;
            owner->rh_ = cmd->y;
            owner->resizeRequest_ = true;
        }

        delete cmd;
    }, new Command { this, w, h });
}

void Window::resize(int w, int h){
    LOG_DEBUG("resized by plugin: " << w << ", " << h);
    // should only be called if the window is open
    if (hwnd_){
        rect_.w = w;
        rect_.h = h;
        adjustRect();
        MoveWindow(hwnd_, rect_.x, rect_.y, rect_.w, rect_.h, TRUE);
        resizeRequest_ = false; // override resize request!
    }
}

void Window::adjustRect(){
    // adjust window dimensions for borders and menu
    const auto style = GetWindowLongPtr(hwnd_, GWL_STYLE);
    const auto exStyle = GetWindowLongPtr(hwnd_, GWL_EXSTYLE);
    const BOOL fMenu = GetMenu(hwnd_) != nullptr;
    RECT rc = { rect_.x, rect_.y, rect_.x + rect_.w, rect_.y + rect_.h };
    AdjustWindowRectEx(&rc, style, fMenu, exStyle);
    rect_.w = rc.right - rc.left;
    rect_.h = rc.bottom - rc.top;
}

void Window::update(){
    if (hwnd_){
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Window::onSizing(RECT& newRect){
    // only called when resizing is enabled!
#if 0
    RECT rc;
    if (GetClientRect(hwnd_, &rc)){
        int dw = rect_.w - (rc.right - rc.left);
        int dh = rect_.h - (rc.bottom - rc.top);
        int w = rect_.w - dw;
        int h = rect_.h - dh;
        // validate requested size
        window->plugin()->checkEditorSize(w, w);
        // TODO: adjust window size
        // editor is resized by WM_SIZE (see Window::procedure)
    }
#endif
}

void Window::onSize(int w, int h){
#if 0
    Rect r { 0, 0, w, h };
    adjustRect(r);
    plugin_->resizeEditor(r.w, r.h);
#else
    plugin_->resizeEditor(w, h);
#endif
}

} // Win32

IWindow::ptr IWindow::create(IPlugin &plugin){
    return std::make_unique<Win32::Window>(plugin);
}

} // vst
