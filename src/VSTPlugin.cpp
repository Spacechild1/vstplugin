#include "VSTPlugin.h"
#include "VST2Plugin.h"

#include <iostream>
#include <thread>

#if _WIN32
# include <windows.h>
#endif
#ifdef DL_OPEN
# include <dlfcn.h>
#endif

#if _WIN32
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
#endif


/*//////////// VST PLUGIN ///////////*/

VSTPlugin::VSTPlugin(const std::string& path)
    : path_(path)
    , win_(nullptr)
{}

VSTPlugin::~VSTPlugin(){
    if (editorThread_.joinable()){
        if(win_) delete win_; win_ = nullptr;
        editorThread_.join();
    }
}

void VSTPlugin::showEditorWindow(){
    if (!hasEditor()){
        std::cout << "plugin doesn't have editor!" << std::endl;
        return;
    }
    // check if message queue is already running
    if (editorThread_.joinable()){
        if(win_) win_->restore();
        return;
    }
    win_ = nullptr;
    editorThread_ = std::thread(&VSTPlugin::threadFunction, this);
}

void VSTPlugin::hideEditorWindow(){
    if (!hasEditor()){
        std::cout << "plugin doesn't have editor!" << std::endl;
        return;
    }
    if (editorThread_.joinable()){
        closeEditor();
        if(win_) delete win_; win_ = nullptr;
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

// private

void VSTPlugin::threadFunction(){
    std::cout << "enter thread" << std::endl;
    win_ = VSTWindowFactory::create("VST Plugin Editor");
    std::cout << "try open editor" << std::endl;
    if(!win_)
      return;

    int left, top, right, bottom;
    openEditor(win_->getHandle());
    std::cout << "opened editor" << std::endl;
    getEditorRect(left, top, right, bottom);
    win_->setGeometry(left, top, right, bottom);
    win_->show();
    win_->top();

    std::cout << "enter message loop!" << std::endl;
    win_->run();
    std::cout << "exit message loop!" << std::endl;

    win_ = nullptr;
}

IVSTPlugin* loadVSTPlugin(const std::string& path){
    AEffect *plugin = nullptr;
    vstPluginFuncPtr mainEntryPoint = NULL;
#if _WIN32
    if (NULL == mainEntryPoint) {
      HMODULE handle = LoadLibraryW(widen(path).c_str());
      if (handle){
        mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "VSTPluginMain"));
        if (mainEntryPoint == NULL){
          mainEntryPoint = (vstPluginFuncPtr)(GetProcAddress(handle, "main"));
        }
      } else {
        std::cout << "loadVSTPlugin: couldn't open " << path << "" << std::endl;
      }
    }
#endif
#ifdef DL_OPEN
    if (NULL == mainEntryPoint) {
      void *handle = dlopen(path.c_str(), RTLD_NOW);
      dlerror();
      if(handle) {
        mainEntryPoint = (vstPluginFuncPtr)(dlsym(handle, "VSTPluginMain"));
        if (mainEntryPoint == NULL){
          mainEntryPoint = (vstPluginFuncPtr)(dlsym(handle, "main"));
        }
      } else {
        std::cout << "loadVSTPlugin: couldn't dlopen " << path << "" << std::endl;
      }
    }
#endif

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
    return new VST2Plugin(plugin, path);
}

void freeVSTPlugin(IVSTPlugin *plugin){
    if (plugin){
        delete plugin;
    }
}
