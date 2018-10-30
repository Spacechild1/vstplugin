#pragma once

#include "VSTPluginInterface.h"

#include <thread>
#include <windows.h>

class VSTPlugin : public IVSTPlugin {
public:
    VSTPlugin(const std::string& path);
    ~VSTPlugin();

    void showEditorWindow() override final;
    void hideEditorWindow() override final;
protected:
    std::string getBaseName() const;
private:
    std::string path_;
    HWND editorHwnd_ = nullptr;
    std::thread editorThread_;
    void threadFunction();
};
