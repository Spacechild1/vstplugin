#pragma once

#include "VSTPluginInterface.h"
#include "VSTWindow.h"
#include <thread>

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
    VSTWindow*win_;
    std::thread editorThread_;

    void threadFunction();
};
