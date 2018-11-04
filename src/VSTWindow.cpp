#include "VSTWindow.h"
VSTWindow* VSTWindow::create(const std::string&name) {
  return 0
#ifdef _FOO
    || (new VSTWindowFoo(name))
#endif
  ;
}

