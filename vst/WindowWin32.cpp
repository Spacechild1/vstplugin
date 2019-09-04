#include "WindowWin32.h"
#include "Utility.h"

#include <cstring>
#include <process.h>

#define VST_EDITOR_CLASS_NAME L"VST Plugin Editor Class"

namespace vst {

std::wstring widen(const std::string& s); // VSTPlugin.cpp

namespace Win32 {
    
namespace UIThread {

#if !HAVE_UI_THREAD
#error "HAVE_UI_THREAD must be defined for Windows!"
// void poll(){}
#endif

IPlugin::ptr create(const PluginInfo& info){
    return EventLoop::instance().create(info);
}

void destroy(IPlugin::ptr plugin){
    EventLoop::instance().destroy(std::move(plugin));
}

bool checkThread(){
    return GetCurrentThreadId() == GetThreadId(EventLoop::instance().threadHandle());
}

EventLoop& EventLoop::instance(){
    static EventLoop thread;
    return thread;
}

DWORD EventLoop::run(void *user){
    auto obj = (EventLoop *)user;
    MSG msg;
    int ret;
    // force message queue creation
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    obj->notify();
    LOG_DEBUG("start message loop");
    while((ret = GetMessage(&msg, NULL, 0, 0))){
        if (ret < 0){
            // error
            LOG_ERROR("GetMessage: error");
            break;
        }
        switch (msg.message){
        case WM_CREATE_PLUGIN:
        {
            LOG_DEBUG("WM_CREATE_PLUGIN");
            auto data = (PluginData *)msg.wParam;
            try {
                auto plugin = data->info->create();
                if (plugin->info().hasEditor()){
                    plugin->setWindow(std::make_unique<Window>(*plugin));
                }
                data->plugin = std::move(plugin);
            } catch (const Error& e){
                data->err = e;
            }
            obj->notify();
            break;
        }
        case WM_DESTROY_PLUGIN:
        {
            LOG_DEBUG("WM_DESTROY_PLUGIN");
            auto data = (PluginData *)msg.wParam;
            data->plugin = nullptr;
            obj->notify();
            break;
        }
        default:
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            break;
        }
    }
    LOG_DEBUG("quit message loop");
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
    std::unique_lock<std::mutex> lock(mutex_);
    // wait for thread to create message queue
    cond_.wait(lock, [&](){ return ready_; });
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

bool EventLoop::postMessage(UINT msg, void *data){
    return PostThreadMessage(threadID_, msg, (WPARAM)data, 0);
}

bool EventLoop::sendMessage(UINT msg, void *data){
    std::unique_lock<std::mutex> lock(mutex_);
    ready_ = false;
    if (postMessage(msg, data)){
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
    if (thread_){
        LOG_DEBUG("create plugin in UI thread");
        PluginData data;
        data.info = &info;
        // notify thread
        if (sendMessage(WM_CREATE_PLUGIN, &data)){
            if (data.plugin){
                return std::move(data.plugin);
            } else {
                throw data.err;
            }
        } else {
            throw Error("couldn't post to thread!");
        }
    } else {
        throw Error("no UI thread!");
    }
}

void EventLoop::destroy(IPlugin::ptr plugin){
    if (thread_){
        PluginData data;
        data.plugin = std::move(plugin);
        // notify thread
        if (!sendMessage(WM_DESTROY_PLUGIN, &data)){
            throw Error("couldn't post to thread!");
        }
    } else {
        throw Error("no UI thread!");
    }
}

} // UIThread

Window::Window(IPlugin& plugin)
    : plugin_(&plugin)
{
    hwnd_ = CreateWindowW(
          VST_EDITOR_CLASS_NAME, L"Untitled",
          WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
          NULL, NULL, NULL, NULL
    );
    LOG_DEBUG("created Window");
    setTitle(plugin_->info().name);
    int left = 100, top = 100, right = 400, bottom = 400;
    plugin_->getEditorRect(left, top, right, bottom);
    setGeometry(left, top, right, bottom);
    LOG_DEBUG("size: " << (right - left) << ", " << (bottom - top));
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
        RECT rc;
        if (GetWindowRect(hWnd, &rc)){
            window->setGeometry(rc.left, rc.top, wParam + rc.left, lParam + rc.top);
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
    } else {
        LOG_ERROR("bug GetWindowLongPtr");
    }
}

void Window::setTitle(const std::string& title){
    SetWindowTextW(hwnd_, widen(title).c_str());
}

void Window::setGeometry(int left, int top, int right, int bottom){
    RECT rc;
    rc.left = left;
    rc.top = top;
    rc.right = right;
    rc.bottom = bottom;
    const auto style = GetWindowLongPtr(hwnd_, GWL_STYLE);
    const auto exStyle = GetWindowLongPtr(hwnd_, GWL_EXSTYLE);
    const BOOL fMenu = GetMenu(hwnd_) != nullptr;
    AdjustWindowRectEx(&rc, style, fMenu, exStyle);
    MoveWindow(hwnd_, left, top, rc.right-rc.left, rc.bottom-rc.top, TRUE);
}

void Window::open(){
    PostMessage(hwnd_, WM_OPEN_EDITOR, 0, 0);
}

void Window::doOpen(){
    ShowWindow(hwnd_, SW_RESTORE);
    BringWindowToTop(hwnd_);
    plugin_->openEditor(getHandle());
    SetTimer(hwnd_, timerID, UIThread::updateInterval, &updateEditor);
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
} // vst
