#pragma once

#include <string>

class VSTWindow {
public:
  VSTWindow(const std::string&name) {;};
  virtual ~VSTWindow(void) = 0;
  virtual void*getHandle(void) = 0; // get system-specific handle to the window (to be passed to effEditOpen)

  virtual void setGeometry(int left, int top, int right, int bottom) = 0;

  virtual void show(void) = 0;
  virtual void restore(void) = 0; // un-minimize

  virtual void top(void) = 0; // bring window to top

  // factory to create a window for the given windowing-system
  static VSTWindow* create(const std::string&name);
};
