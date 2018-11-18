#include "VSTWindowWin32.h"
#include "Utility.h"

#include <cstring>

#define VST_EDITOR_CLASS_NAME L"VST Plugin Editor Class"

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

namespace VSTWindowFactory {
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
}

VSTWindowWin32::VSTWindowWin32(){
    hwnd_ = CreateWindowW(
          VST_EDITOR_CLASS_NAME, L"Untitled",
          WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
          NULL, NULL, NULL, NULL
    );
    LOG_DEBUG("created VSTWindowWin32");
}

VSTWindowWin32::~VSTWindowWin32(){
    PostMessage(hwnd_, WM_DESTROY, 0, 0);
    LOG_DEBUG("destroyed VSTWindowWin32");
}

void VSTWindowWin32::run(){
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
}

void VSTWindowWin32::setTitle(const std::string& title){
    SetWindowTextW(hwnd_, widen(title).c_str());
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

namespace VSTWindowFactory {
    IVSTWindow* createWin32() {
        return new VSTWindowWin32();
    }
}
