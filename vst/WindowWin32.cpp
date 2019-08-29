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

bool check(){
    return GetCurrentThreadId() == GetThreadId(EventLoop::instance().threadHandle());
}

EventLoop& EventLoop::instance(){
    static EventLoop thread;
    return thread;
}

static LRESULT WINAPI PluginEditorProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam){
    if (Msg == WM_CLOSE){
        ShowWindow(hWnd, SW_HIDE); // don't destroy Window when closed
        return true;
    }
    if (Msg == WM_DESTROY){
        LOG_DEBUG("WM_DESTROY");
    }
    return DefWindowProcW(hWnd, Msg, wParam, lParam);
}

DWORD EventLoop::run(void *user){
    auto obj = (EventLoop *)user;
    MSG msg;
    int ret;
    // force message queue creation
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    {
        std::lock_guard<std::mutex> lock(obj->mutex_);
        obj->ready_ = true;
        obj->cond_.notify_one();
    }
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
            std::unique_lock<std::mutex> lock(obj->mutex_);
            try {
                auto plugin = obj->info_->create();
                if (plugin->info().hasEditor()){
                    plugin->setWindow(std::make_unique<Window>(*plugin));
                }
                obj->plugin_ = std::move(plugin);
            } catch (const Error& e){
                obj->err_ = e;
                obj->plugin_ = nullptr;
            }
            obj->info_ = nullptr; // done
            lock.unlock();
            obj->cond_.notify_one();
            break;
        }
        case WM_DESTROY_PLUGIN:
        {
            LOG_DEBUG("WM_DESTROY_PLUGIN");
            std::unique_lock<std::mutex> lock(obj->mutex_);
            obj->plugin_ = nullptr;
            lock.unlock();
            obj->cond_.notify_one();
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
    wcex.lpfnWndProc = PluginEditorProc;
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

bool EventLoop::postMessage(UINT msg, WPARAM wparam, LPARAM lparam){
    return PostThreadMessage(threadID_, msg, wparam, lparam);
}

IPlugin::ptr EventLoop::create(const PluginInfo& info){
    if (thread_){
        LOG_DEBUG("create plugin in UI thread");
        std::unique_lock<std::mutex> lock(mutex_);
        info_ = &info;
        // notify thread
        if (postMessage(WM_CREATE_PLUGIN)){
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
            throw Error("couldn't post to thread!");
        }
    } else {
        throw Error("no UI thread!");
    }
}

void EventLoop::destroy(IPlugin::ptr plugin){
    if (thread_){
        std::unique_lock<std::mutex> lock(mutex_);
        plugin_ = std::move(plugin);
        // notify thread
        if (postMessage(WM_DESTROY_PLUGIN)){
            // wait till done
            cond_.wait(lock, [&]{return plugin_ == nullptr;});
        } else {
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
    plugin_->openEditor(getHandle());
}

Window::~Window(){
    plugin_->closeEditor();
    DestroyWindow(hwnd_);
    LOG_DEBUG("destroyed Window");
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
    MoveWindow(hwnd_, 0, 0, rc.right-rc.left, rc.bottom-rc.top, TRUE);
}

void Window::show(){
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

void Window::hide(){
    ShowWindow(hwnd_, SW_HIDE);
    UpdateWindow(hwnd_);
}

void Window::minimize(){
    ShowWindow(hwnd_, SW_MINIMIZE);
    UpdateWindow(hwnd_);
}

void Window::restore(){
    ShowWindow(hwnd_, SW_RESTORE);
    BringWindowToTop(hwnd_);
}

void Window::bringToTop(){
    minimize();
    restore();
}

void Window::update(){
    InvalidateRect(hwnd_, nullptr, FALSE);
}
} // Win32
} // vst
