#pragma once

#include "VSTPluginInterface.h"

#include <thread>
#include <windows.h>

class VSTPlugin : public IVSTPlugin {
public:
    VSTPlugin(const std::string& path);
    ~VSTPlugin();
    std::string getPluginName() const override final;

    void showEditorWindow() override final;
    void hideEditorWindow() override final;
private:
    std::string name_;
    HWND editorHwnd_ = nullptr;
    std::thread editorThread_;
    void threadFunction();
};
