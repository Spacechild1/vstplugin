#include "VSTPlugin.h"
#include "VST2Plugin.h"

#include <process.h>
#include <iostream>
#include <thread>

// #include "tchar.h"

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
static std::string shorten(const std::wstring& s){
    if (s.empty()){
        return std::string();
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), s.size(), NULL, 0, NULL, NULL);
    std::string buf;
    buf.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), s.size(), &buf[0], n, NULL, NULL);
    return buf;
}

/*////////// EDITOR WINDOW //////////*/

static LRESULT WINAPI VSTPluginEditorProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam){
    if (Msg == WM_DESTROY){
        PostQuitMessage(0);
    }
    return DefWindowProcW(hWnd, Msg, wParam, lParam);
}

void restoreWindow(HWND hwnd){
    if (hwnd){
        ShowWindow(hwnd, SW_RESTORE);
        BringWindowToTop(hwnd);
    }
}

void closeWindow(HWND hwnd){
    if (hwnd){
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }
}

static void setWindowGeometry(HWND hwnd, int left, int top, int right, int bottom){
    if (hwnd) {
        RECT rc;
        rc.left = left;
        rc.top = top;
        rc.right = right;
        rc.bottom = bottom;
        const auto style = GetWindowLongPtr(hwnd, GWL_STYLE);
        const auto exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        const BOOL fMenu = GetMenu(hwnd) != nullptr;
        AdjustWindowRectEx(&rc, style, fMenu, exStyle);
        MoveWindow(hwnd, 0, 0, rc.right-rc.left, rc.bottom-rc.top, TRUE);
        // SetWindowPos(hwnd, HWND_TOP, 0, 0, rc.right-rc.left, rc.bottom-rc.top, 0);
        std::cout << "resized Window to " << left << ", " << top << ", " << right << ", " << bottom << std::endl;
    }
}

/*/////////// DLL MAIN //////////////*/

static HINSTANCE hInstance = NULL;
static WNDCLASSEXW VSTWindowClass;
static bool bRegistered = false;

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

/*//////////// VST PLUGIN ///////////*/

VSTPlugin::VSTPlugin(const std::string& path)
    : path_(path){}

VSTPlugin::~VSTPlugin(){
    if (isEditorOpen()){
        closeWindow(editorHwnd_);
    }
    if (editorThread_.joinable()){
        editorThread_.join();
    }
}

void VSTPlugin::showEditorWindow(){
    if (!hasEditor()){
        std::cout << "plugin doesn't have editor!" << std::endl;
        return;
    }
    // check if editor is already open
    if (isEditorOpen()){
        restoreWindow(editorHwnd_);
        return;
    }
    if (editorThread_.joinable()){  // Window has been closed
        editorThread_.join();
    }
    editorThread_ = std::thread(&VSTPlugin::threadFunction, this);
    editorOpen_.store(true);
}

void VSTPlugin::hideEditorWindow(){
    if (!hasEditor()){
        std::cout << "plugin doesn't have editor!" << std::endl;
        return;
    }
    if (isEditorOpen()){
        closeEditor();
        closeWindow(editorHwnd_);
    }
    if (editorThread_.joinable()){
        editorThread_.join();
    }
}

// protected
std::string VSTPlugin::getBaseName() const {
    auto sep = path_.find_last_of("\\/");
    auto dot = path_.find_last_of('.');
    if (sep == std::string::npos){
        sep = -1;
    }
    if (dot == std::string::npos){
        dot = path_.size();
    }
    return path_.substr(sep + 1, dot - sep - 1);
}

bool VSTPlugin::isEditorOpen() const {
    return editorOpen_.load();
}

// private

void VSTPlugin::threadFunction(){
    std::cout << "enter thread" << std::endl;
    editorHwnd_ = CreateWindowW(
        VSTWindowClass.lpszClassName, L"VST Plugin Editor",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
        NULL, NULL, hInstance, NULL
    );

    std::cout << "try open editor" << std::endl;
    openEditor(editorHwnd_);
    std::cout << "opened editor" << std::endl;
    int left, top, right, bottom;
    getEditorRect(left, top, right, bottom);
    setWindowGeometry(editorHwnd_, left, top, right, bottom);

    ShowWindow(editorHwnd_, SW_SHOW);
    UpdateWindow(editorHwnd_);

    // hack to bring Window to the top
    ShowWindow(editorHwnd_, SW_MINIMIZE);
    restoreWindow(editorHwnd_);

    std::cout << "enter message loop!" << std::endl;
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
    std::cout << "exit message loop!" << std::endl;
    editorHwnd_ = nullptr;
    editorOpen_.store(false);
}

IVSTPlugin* loadVSTPlugin(const std::string& path){
    AEffect *plugin = nullptr;
    HMODULE handle = nullptr;
    // check for extension
    auto ext = path.find_last_of('.');
    if (ext != std::string::npos &&
            ((path.find(".dll", ext) != std::string::npos)
             || path.find(".DLL", ext) != std::string::npos)){
        handle = LoadLibraryW(widen(path).c_str());
    } else { // add extension
        wchar_t buf[MAX_PATH];
        snwprintf(buf, MAX_PATH, L"%S.dll", widen(path).c_str());
        handle = LoadLibraryW(buf);
    }

    if (!handle){
        std::cout << "loadVSTPlugin: couldn't open " << path << "" << std::endl;
        return nullptr;
    }
    vstPluginFuncPtr mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "VSTPluginMain"));
    if (!mainEntryPoint){
        mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "main"));
    }
    if (!mainEntryPoint){
        std::cout << "loadVSTPlugin: couldn't find entry point in VST plugin" << std::endl;
        return nullptr;
    }
    plugin = mainEntryPoint(&VST2Plugin::hostCallback);
    if (!plugin){
        std::cout << "loadVSTPlugin: couldn't initialize plugin" << std::endl;
        return nullptr;
    }
    if (plugin->magic != kEffectMagic){
        std::cout << "loadVSTPlugin: bad magic number!" << std::endl;
        return nullptr;
    }
    std::cout << "loadVSTPlugin: successfully loaded plugin" << std::endl;
    return new VST2Plugin(plugin, path);
}

void freeVSTPlugin(IVSTPlugin *plugin){
    if (plugin){
        delete plugin;
    }
}
