#pragma once

#include "VSTPluginInterface.h"
#include "VSTWindow.h"
#include <thread>
#include <atomic>

class VSTPlugin : public IVSTPlugin {
public:
    VSTPlugin(const std::string& path);
    ~VSTPlugin();

    void showEditorWindow() override final;
    void hideEditorWindow() override final;
protected:
    std::string getBaseName() const;
    bool isEditorOpen() const;
private:
    std::string path_;
    VSTWindow* win_{nullptr};
    std::thread editorThread_;
    std::atomic<bool> editorOpen_{false};
    void threadFunction();
};
