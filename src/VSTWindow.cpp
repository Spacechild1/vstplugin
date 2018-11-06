#include "VSTWindow.h"
namespace VSTWindowFactory {
#ifdef _WIN32
    VSTWindow* createWin32(IVSTPlugin& plugin);
#endif
#ifdef USE_WINDOW_FOO
    VSTWindow* createFoo(IVSTPlugin& plugin);
#endif

    VSTWindow* create(IVSTPlugin& plugin){
        VSTWindow *win = nullptr;
    #ifdef _WIN32
        win = createWin32(plugin);
    #endif
    #ifdef USE_WINDOW_FOO
        win = createFoo(plugin);
    #endif
        return win;
    }
}
