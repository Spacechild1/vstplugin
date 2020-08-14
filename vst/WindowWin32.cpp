#include "WindowWin32.h"
#include "Utility.h"

#include <cstring>
#include <process.h>

#define VST_EDITOR_CLASS_NAME L"VST Plugin Editor Class"

namespace vst {

namespace UIThread {

void setup(){}

void run(){}

void quit(){}

bool isCurrentThread(){
    return Win32::EventLoop::instance().checkThread();
}

void poll(){}

bool callSync(Callback cb, void *user){
    return Win32::EventLoop::instance().sendMessage(Win32::WM_CALL, (void *)cb, user);
}

bool callAsync(Callback cb, void *user){
    return Win32::EventLoop::instance().postMessage(Win32::WM_CALL, (void *)cb, user);
}

int32_t addPollFunction(PollFunction fn, void *context){
    return Win32::EventLoop::instance().addPollFunction(fn, context);
}

void removePollFunction(int32_t handle){
    return Win32::EventLoop::instance().removePollFunction(handle);
}

} // UIThread

namespace Win32 {

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

    for (;;){
        if (GetMessage(&msg, NULL, 0, 0) < 0){
            LOG_ERROR("GetMessage: error");
            break;
        }

        if (msg.message == WM_CALL){
            LOG_DEBUG("WM_CREATE_PLUGIN");
            auto cb = (UIThread::Callback)msg.wParam;
            auto data = (void *)msg.lParam;
            cb(data);
            obj->notify();
        } else if ((msg.message == WM_TIMER) && (msg.hwnd == NULL)
                   && (msg.wParam == timer)) {
            // call poll functions
            std::lock_guard<std::mutex> lock(obj->pollFunctionMutex_);
            for (auto& it : obj->pollFunctions_){
                it.second();
            }
            // LOG_VERBOSE("call poll functions.");
        } else {
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
    if (thread_){
        if (postMessage(WM_QUIT)){
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
            LOG_DEBUG("joined thread");
        } else {
            LOG_ERROR("couldn't post quit message!");
        }
    }
}

bool EventLoop::checkThread() {
    return GetCurrentThreadId() == threadID_;
}

bool EventLoop::postMessage(UINT msg, void *data1, void *data2){
    return PostThreadMessage(threadID_, msg, (WPARAM)data1, (LPARAM)data2);
}

bool EventLoop::sendMessage(UINT msg, void *data1, void *data2){
    std::lock_guard<std::mutex> lock(mutex_); // prevent concurrent calls
    if (postMessage(msg, data1, data2)){
        LOG_DEBUG("waiting...");
        event_.wait();
        LOG_DEBUG("done");
        return true;
    } else {
        return false;
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
    event_.signal();
}

Window::Window(IPlugin& plugin)
    : plugin_(&plugin)
{
    // no maximize box if plugin view can't be resized
    DWORD dwStyle = plugin.canResize() ? WS_OVERLAPPEDWINDOW : WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;
    hwnd_ = CreateWindowW(
          VST_EDITOR_CLASS_NAME, L"Untitled",
          dwStyle, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
          NULL, NULL, NULL, NULL
    );
    LOG_DEBUG("created Window");
    // set window title
    SetWindowTextW(hwnd_, widen(plugin.info().name).c_str());
    // get window dimensions from plugin
    int left = 100, top = 100, right = 400, bottom = 400;
    if (!plugin_->getEditorRect(left, top, right, bottom)){
        // HACK for plugins which don't report the window size without the editor being opened
        LOG_DEBUG("couldn't get editor rect!");
        plugin_->openEditor(hwnd_);
        plugin_->getEditorRect(left, top, right, bottom);
        plugin_->closeEditor();
    }
    LOG_DEBUG("window size: " << (right - left) << " * " << (bottom - top));
    RECT rc = { left, top, right, bottom };
    // adjust window dimensions for borders and menu
    const auto style = GetWindowLongPtr(hwnd_, GWL_STYLE);
    const auto exStyle = GetWindowLongPtr(hwnd_, GWL_EXSTYLE);
    const BOOL fMenu = GetMenu(hwnd_) != nullptr;
    AdjustWindowRectEx(&rc, style, fMenu, exStyle);
    // set window dimensions
    MoveWindow(hwnd_, left, top, rc.right-rc.left, rc.bottom-rc.top, TRUE);
    // set user data
    SetWindowLongPtr(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
}

Window::~Window(){
    KillTimer(hwnd_, timerID);
    plugin_->closeEditor();
    DestroyWindow(hwnd_);
    LOG_DEBUG("destroyed Window");
}

LRESULT WINAPI Window::procedure(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam){
    auto window = (Window *)GetWindowLongPtr(hWnd, GWLP_USERDATA);

    switch (Msg){
    case WM_CLOSE: // intercept close event!
    case WM_CLOSE_EDITOR:
    {
        if (window){
            window->doClose();
        } else {
            LOG_ERROR("bug GetWindowLongPtr");
        }
        return true;
    }
    case WM_OPEN_EDITOR:
    {
        if (window){
            window->doOpen();
        } else {
            LOG_ERROR("bug GetWindowLongPtr");
        }
        return true;
    }
    case WM_EDITOR_POS:
    {
        RECT rc;
        if (GetWindowRect(hWnd, &rc)){
            MoveWindow(hWnd, wParam, lParam, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        }
        return true;
    }
    case WM_EDITOR_SIZE:
    {
        RECT rc, client;
        if (GetWindowRect(hWnd, &rc) && GetClientRect(hWnd, &client)){
            int w = wParam + (rc.right - rc.left) - (client.right - client.left);
            int h = lParam + (rc.bottom - rc.top) - (client.bottom - client.top);
            MoveWindow(hWnd, rc.left, rc.top, w, h, TRUE);
        }
        return true;
    }
    case WM_SIZING:
    {
        auto newRect = (RECT *)lParam;
        RECT oldRect;
        if (GetWindowRect(hWnd, &oldRect)){
            if (window && window->plugin()->canResize()){
                RECT clientRect;
                if (GetClientRect(hWnd, &clientRect)){
                    int dw = (oldRect.right - oldRect.left) - (clientRect.right - clientRect.left);
                    int dh = (oldRect.bottom - oldRect.top) - (clientRect.bottom - clientRect.top);
                    int width = (newRect->right - newRect->left) - dw;
                    int height = (newRect->bottom - newRect->top) - dh;
                #if 0
                    // validate requested size
                    window->plugin()->checkEditorSize(width, height);
                    // TODO: adjust window size
                #endif
                    // editor is resized by WM_SIZE (see below)
                }
            } else {
                // bash to old size
                *newRect = oldRect;
            }
        }
        return true;
    }
    case WM_SIZE:
    {
        if (wParam == SIZE_MAXIMIZED || wParam == SIZE_RESTORED){
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            if (window) {
                window->plugin()->resizeEditor(w, h);
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
    PostMessage(hwnd_, WM_OPEN_EDITOR, 0, 0);
}

void Window::doOpen(){
    ShowWindow(hwnd_, SW_RESTORE);
    BringWindowToTop(hwnd_);
    plugin_->openEditor(getHandle());
    SetTimer(hwnd_, timerID, EventLoop::updateInterval, &updateEditor);
}

void Window::close(){
    PostMessage(hwnd_, WM_CLOSE_EDITOR, 0, 0);
}

void Window::doClose(){
    KillTimer(hwnd_, timerID);
    plugin_->closeEditor();
    ShowWindow(hwnd_, SW_HIDE);
}

void Window::setPos(int x, int y){
    PostMessage(hwnd_, WM_EDITOR_POS, x, y);
}

void Window::setSize(int w, int h){
    LOG_DEBUG("new size: " << w << ", " << h);
    PostMessage(hwnd_, WM_EDITOR_SIZE, w, h);
}

void Window::update(){
    InvalidateRect(hwnd_, nullptr, FALSE);
}
} // Win32

IWindow::ptr IWindow::create(IPlugin &plugin){
    return std::make_unique<Win32::Window>(plugin);
}

} // vst
