#include "WindowWin32.h"
#include "Utility.h"

#include <cstring>
#include <process.h>

#define VST_EDITOR_CLASS_NAME L"VST Plugin Editor Class"

namespace vst {

std::wstring widen(const std::string& s); // VSTPlugin.cpp

static LRESULT WINAPI VSTPluginEditorProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam){
    if (Msg == WM_CLOSE){
        ShowWindow(hWnd, SW_HIDE); // don't destroy Window when closed
        return true;
    }
    if (Msg == WM_DESTROY){
        PostQuitMessage(0);
        LOG_DEBUG("WM_DESTROY");
    }
    return DefWindowProcW(hWnd, Msg, wParam, lParam);
}

namespace WindowFactory {
    void initializeWin32(){
        static bool initialized = false;
        if (!initialized){
            WNDCLASSEXW wcex;
            memset(&wcex, 0, sizeof(WNDCLASSEXW));
            wcex.cbSize = sizeof(WNDCLASSEXW);
            wcex.lpfnWndProc = VSTPluginEditorProc;
            wcex.lpszClassName =  VST_EDITOR_CLASS_NAME;
            wchar_t exeFileName[MAX_PATH];
            GetModuleFileNameW(NULL, exeFileName, MAX_PATH);
            wcex.hIcon = ExtractIconW(NULL, exeFileName, 0);
            if (!RegisterClassExW(&wcex)){
                LOG_WARNING("couldn't register window class!");
            } else {
                LOG_DEBUG("registered window class!");
                initialized = true;
            }
        }
    }

    IWindow::ptr createWin32(IPlugin::ptr plugin){
        return std::make_shared<WindowWin32>(std::move(plugin));
    }
}

WindowWin32::WindowWin32(IPlugin::ptr plugin)
    : plugin_(std::move(plugin))
{
    hwnd_ = CreateWindowW(
          VST_EDITOR_CLASS_NAME, L"Untitled",
          WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
          NULL, NULL, NULL, NULL
    );
    LOG_DEBUG("created WindowWin32");
}

WindowWin32::~WindowWin32(){
    DestroyWindow(hwnd_);
    LOG_DEBUG("destroyed WindowWin32");
}

void WindowWin32::run(){
	MSG msg;
    int ret;
    while((ret = GetMessage(&msg, NULL, 0, 0))){
        if (ret < 0){
            // error
            LOG_WARNING("GetMessage: error");
            break;
        }
        DispatchMessage(&msg);
    }
        // close the editor here (in the GUI thread).
        // some plugins depend on this.
    plugin_->closeEditor();
}

void WindowWin32::quit(){
    PostMessage(hwnd_, WM_QUIT, 0, 0);
}

void WindowWin32::setTitle(const std::string& title){
    SetWindowTextW(hwnd_, widen(title).c_str());
}

void WindowWin32::setGeometry(int left, int top, int right, int bottom){
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

void WindowWin32::show(){
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

void WindowWin32::hide(){
    ShowWindow(hwnd_, SW_HIDE);
    UpdateWindow(hwnd_);
}

void WindowWin32::minimize(){
    ShowWindow(hwnd_, SW_MINIMIZE);
    UpdateWindow(hwnd_);
}

void WindowWin32::restore(){
    ShowWindow(hwnd_, SW_RESTORE);
    BringWindowToTop(hwnd_);
}

void WindowWin32::bringToTop(){
    minimize();
    restore();
}

void WindowWin32::update(){
    InvalidateRect(hwnd_, nullptr, FALSE);
}

} // vst
