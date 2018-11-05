#include "VSTWindow.h"
VSTWindow* VSTWindow::create(const std::string&name) {
  return 0
#ifdef _WIN32
    || (new VSTWindowWin32(name))
#endif
#ifdef _FOO
    || (new VSTWindowFoo(name))
#endif
  ;
}

