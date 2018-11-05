#include "VSTWindow.h"

#include <windows.h>
#include <process.h>

#include <iostream>

static HINSTANCE hInstance = NULL;
static WNDCLASSEXW VSTWindowClass;
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
      memset(&VSTWindowClass, 0, sizeof(WNDCLASSEXW));
      VSTWindowClass.cbSize = sizeof(WNDCLASSEXW);
      VSTWindowClass.lpfnWndProc = VSTPluginEditorProc;
      VSTWindowClass.hInstance = hInstance;
      VSTWindowClass.lpszClassName = L"VST Plugin Editor Class";
      if (!RegisterClassExW(&VSTWindowClass)){
        std::cout << "couldn't register window class!" << std::endl;
      } else {
        std::cout << "registered window class!" << std::endl;
        bRegistered = true;
      }
    }
    return TRUE;
  }
} // extern C


class VSTWindowWin32 : public VSTWindow {
private:
  HWND hwnd_ = nullptr;
public:
  VSTWindowWin32(const std::string&name)
    : hwnd_(CreateWindowW(
              VSTWindowClass.lpszClassName, L"VST Plugin Editor",
              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
              NULL, NULL, hInstance, NULL
              ))
    {
    }
  ~VSTWindowWin32(void) {
    PostMessage(hwnd_, WM_CLOSE, 0, 0);
  }
  void*getHandle(void) {
    return hwnd_;
  }

  void setGeometry(int left, int top, int right, int bottom) {
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
    // SetWindowPos(hwnd_, HWND_TOP, 0, 0, rc.right-rc.left, rc.bottom-rc.top, 0);
    std::cout << "resized Window to " << left << ", " << top << ", " << right << ", " << bottom << std::endl;
  }
  void restore(void) {
    ShowWindow(hwnd_, SW_RESTORE);
    BringWindowToTop(hwnd_);
  }

  void top(void) {
    ShowWindow(hwnd_, SW_MINIMIZE);
    restore();
  }
  void show(void) {
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
  }

  void run(void) {
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
};

namespace VSTWindowFactory {
  VSTWindow* createWin32(const std::string&name) {
    return new VSTWindowWin32(name);
  }
}
