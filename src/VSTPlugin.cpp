#include "VSTPlugin.h"
#include "VST2Plugin.h"

#include <process.h>
#include <iostream>

HINSTANCE hInstance = NULL;
WNDCLASSEXW VSTWindowClass;

extern "C" {
BOOL WINAPI DllMain(HINSTANCE hinstDLL,DWORD fdwReason, LPVOID lpvReserved){
    hInstance = hinstDLL;

    if (fdwReason != DLL_THREAD_ATTACH && fdwReason != DLL_THREAD_DETACH){
        memset(&VSTWindowClass, 0, sizeof(WNDCLASSEXW));
        VSTWindowClass.cbSize = sizeof(WNDCLASSEXW);
        VSTWindowClass.lpfnWndProc    = DefWindowProc;
        VSTWindowClass.hInstance      = hInstance;
        VSTWindowClass.lpszClassName  = L"VST Editor Test Class";
        if (!RegisterClassExW(&VSTWindowClass)){
            std::cout << "couldn't register window class!" << std::endl;
        } else {
            std::cout << "registered window class!" << std::endl;
        }
    }
    return TRUE;
}
} // extern C

static void resizeWindow(HWND hwnd, int left, int top, int right, int bottom);

VSTPlugin::~VSTPlugin(){
    if (editorHwnd_){
        SendMessage(editorHwnd_, WM_CLOSE, 0, 0);
        editorHwnd_ = nullptr;
    }
}

void VSTPlugin::createEditorWindow(){
    if (!hasEditor()){
        std::cout << "plugin doesn't have editor!" << std::endl;
        return;
    }
    if (editorHwnd_){
        return;
    }
    std::cout << "show editor" << std::endl;
    auto threadFunc = [this](){
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
        resizeWindow(editorHwnd_, left, top, right, bottom);

        ShowWindow(editorHwnd_, SW_SHOW);
        UpdateWindow(editorHwnd_);

        std::cout << "enter message loop!" << std::endl;
        MSG msg;
        int ret;
        while((ret = GetMessage(&msg, NULL, 0, 0))){
            if (ret < 0){
                std::cout << "GetMessage: error" << std::endl;
                // error
                break;
            }
            // std::cout << "message: " << msg.message << std::endl;
            switch (msg.message){
            case WM_CLOSE:
                std::cout << "closed window" << std::endl;
                break;
            case WM_DESTROY:
                std::cout << "destroyed window" << std::endl;
                break;
            default:
                break;
            }
            DispatchMessage(&msg);
            // std::cout << "result: " <<  << std::endl;

        }
        editorHwnd_ = NULL;
        std::cout << "exit message loop!" << std::endl;
    };
    editorThread_ = std::thread(threadFunc);
    editorThread_.detach();
    std::cout << "detached thread" << std::endl;
}

void VSTPlugin::destroyEditorWindow(){
    if (!hasEditor()){
        std::cout << "plugin doesn't have editor!" << std::endl;
        return;
    }
    if (!editorHwnd_){
        return;
    }
    closeEditor();
    SendMessage(editorHwnd_, WM_CLOSE, 0, 0);
    editorHwnd_ = nullptr;
}

static void resizeWindow(HWND hwnd, int left, int top, int right, int bottom){
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
        std::cout << "resized Window to " << left << ", " << top << ", " << right << ", " << bottom << std::endl;
    }
}

IVSTPlugin* loadVSTPlugin(const std::string& path){
    AEffect *plugin = nullptr;
    HMODULE handle = LoadLibraryA(path.c_str());
    if (handle == NULL){
        std::cout << "loadVSTPlugin: couldn't open " << path << "" << std::endl;
        return nullptr;
    }
    vstPluginFuncPtr mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "VSTPluginMain"));
    if (mainEntryPoint == NULL){
        mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "main"));
    }
    if (mainEntryPoint == NULL){
        std::cout << "loadVSTPlugin: couldn't find entry point in VST plugin" << std::endl;
        return nullptr;
    }
    plugin = mainEntryPoint(&VST2Plugin::hostCallback);
    if (plugin == NULL){
        std::cout << "loadVSTPlugin: couldn't initialize plugin" << std::endl;
        return nullptr;
    }
    if (plugin->magic != kEffectMagic){
        std::cout << "loadVSTPlugin: bad magic number!" << std::endl;
        return nullptr;
    }
    std::cout << "loadVSTPlugin: successfully loaded plugin" << std::endl;
    return new VST2Plugin(plugin);
}

void freeVSTPlugin(IVSTPlugin *plugin){
    if (plugin){
        delete plugin;
    }
}
