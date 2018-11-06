#include "VSTWindowWin32.h"

#include <iostream>

static std::wstring widen(const std::string& s){
    if (s.empty()){
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), s.size(), NULL, 0);
    std::wstring buf;
    buf.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), s.size(), &buf[0], n);
    return buf;
}

#define VST_EDITOR_CLASS_NAME L"VST Plugin Editor Class"
static HINSTANCE hInstance = NULL;
static bool bRegistered = false;

static LRESULT WINAPI VSTPluginEditorProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam){
    if (Msg == WM_DESTROY){
        PostQuitMessage(0);
    }
    return DefWindowProcW(hWnd, Msg, wParam, lParam);
}


extern "C" {
BOOL WINAPI DllMain(HINSTANCE hinstDLL,DWORD fdwReason, LPVOID lpvReserved){
    hInstance = hinstDLL;

    if (!bRegistered){
        WNDCLASSEXW wcex;
        memset(&wcex, 0, sizeof(WNDCLASSEXW));
        wcex.cbSize = sizeof(WNDCLASSEXW);
        wcex.lpfnWndProc = VSTPluginEditorProc;
        wcex.hInstance = hInstance;
        wcex.lpszClassName =  VST_EDITOR_CLASS_NAME;
        if (!RegisterClassExW(&wcex)){
            std::cout << "couldn't register window class!" << std::endl;
        } else {
            std::cout << "registered window class!" << std::endl;
            bRegistered = true;
        }
    }
    return TRUE;
}
} // extern C


VSTWindowWin32::VSTWindowWin32(IVSTPlugin& plugin){
    bRunning_.store(true);
    thread_ = std::thread(&VSTWindowWin32::threadFunction, this, &plugin);
    std::cout << "created VSTWindowWin32" << std::endl;
}

VSTWindowWin32::~VSTWindowWin32(){
    if (isRunning()){
        PostMessage(hwnd_, WM_CLOSE, 0, 0);
    }
    if (thread_.joinable()){
        thread_.join();
    }
    std::cout << "destroyed VSTWindowWin32" << std::endl;
}

void VSTWindowWin32::setGeometry(int left, int top, int right, int bottom){
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

void VSTWindowWin32::show(){
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

void VSTWindowWin32::hide(){
    ShowWindow(hwnd_, SW_HIDE);
    UpdateWindow(hwnd_);
}

void VSTWindowWin32::minimize(){
    ShowWindow(hwnd_, SW_MINIMIZE);
    UpdateWindow(hwnd_);
}

void VSTWindowWin32::restore(){
    ShowWindow(hwnd_, SW_RESTORE);
    BringWindowToTop(hwnd_);
}

void VSTWindowWin32::bringToTop(){
    minimize();
    restore();
}

void VSTWindowWin32::run(){
    MSG msg;
    int ret;
    while((ret = GetMessage(&msg, NULL, 0, 0))){
        if (ret < 0){
            // error
            std::cout << "GetMessage: error" << std::endl;
            break;
        }
        DispatchMessage(&msg);
    }
}

void VSTWindowWin32::threadFunction(IVSTPlugin *plugin){
    std::cout << "enter thread" << std::endl;
    hwnd_ = CreateWindowW(
          VST_EDITOR_CLASS_NAME, widen(plugin->getPluginName()).c_str(),
          WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
          NULL, NULL, hInstance, NULL
    );

    int left, top, right, bottom;
    std::cout << "try open editor" << std::endl;
    plugin->openEditor(getHandle());
    std::cout << "opened editor" << std::endl;
    plugin->getEditorRect(left, top, right, bottom);
    setGeometry(left, top, right, bottom);
    show();
    bringToTop();

    std::cout << "enter message loop!" << std::endl;
    run();
    plugin->closeEditor();
    bRunning_.store(false);
    std::cout << "exit message loop!" << std::endl;
}

namespace VSTWindowFactory {
    IVSTWindow* createWin32(IVSTPlugin& plugin) {
        return new VSTWindowWin32(plugin);
    }
}
