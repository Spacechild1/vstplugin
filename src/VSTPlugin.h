#pragma once

#include "VSTPluginInterface.h"

#include <memory>

class VSTPlugin : public IVSTPlugin {
 public:
    VSTPlugin(const std::string& path);

    void createWindow() override final;
    void destroyWindow() override final;
    IVSTWindow *getWindow() override final {
        return win_.get();
    }
 protected:
    std::string getBaseName() const;
 private:
    std::string path_;
    std::unique_ptr<IVSTWindow> win_;
};

namespace VSTWindowFactory {
    IVSTWindow* create(IVSTPlugin& plugin);
}
