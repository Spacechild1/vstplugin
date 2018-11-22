# Makefile to build library 'vsthost~' for Pure Data.
# Needs Makefile.pdlibbuilder as helper makefile for platform-dependent build
# settings and rules.
#
# use : make PDDIR=/path/to/pure-data
#
# The following command will build the external and install the distributable 
# files into a subdirectory called build :
#
# make install PDDIR=/path/to/pure-data PDLIBDIR=./build

lib.name = vsthost~

class.sources = src/vsthost~.cpp

common.sources = src/VSTPlugin.cpp src/VST2Plugin.cpp

VST2DIR = src/VST_SDK/VST2_SDK/pluginterfaces/vst2.x/

cflags = -Wno-unused -Wno-unused-parameter \
	-std=c++11 \
	-g \
	-I"${VST2DIR}" \
	-DLOGLEVEL=3 \
	$(empty)

define forWindows
  common.sources += src/VSTWindowWin32.cpp
  cflags += -DVSTTHREADS=1
endef
define forLinux
  common.sources += src/VSTWindowX11.cpp
  cflags += -DTARGET_API_MAC_CARBON=1 -DDL_OPEN=1 -DUSE_X11=1 -DVSTTHREADS=1
  ldflags += -L/usr/X11R6/lib -lX11
endef
define forDarwin
 common.sources += src/VSTWindowCocoa.mm
 cflags += -fno-objc-arc -DVSTTHREADS=0
 ldflags += -framework Cocoa
endef


# all extra files to be included in binary distribution of the library
datafiles = vsthost~-help.pd

include pd-lib-builder/Makefile.pdlibbuilder
