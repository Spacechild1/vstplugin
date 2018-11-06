#pragma once

#include "VSTPluginInterface.h"
#include "VSTWindow.h"

#include <memory>

class VSTPlugin : public IVSTPlugin {
public:
    VSTPlugin(const std::string& path);

    void showEditorWindow() override final;
    void hideEditorWindow() override final;
protected:
    std::string getBaseName() const;
private:
    std::string path_;
    std::unique_ptr<VSTWindow> win_;
};
