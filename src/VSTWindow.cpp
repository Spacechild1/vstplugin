#include "VSTWindow.h"
namespace VSTWindowFactory {
#ifdef _WIN32
    VSTWindow* createWin32(const std::string&name);
#endif
#ifdef USE_WINDOW_FOO
    VSTWindow* createFoo(const std::string&name);
#endif

    VSTWindow* create(const std::string&name){
        VSTWindow *win = nullptr;
    #ifdef _WIN32
        win = createWin32(name);
    #endif
    #ifdef USE_WINDOW_FOO
        win = createFoo(name);
    #endif
        return win;
    }
}
