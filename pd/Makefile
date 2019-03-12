# Makefile to build library 'vstplugin~' for Pure Data.
# Needs Makefile.pdlibbuilder as helper makefile for platform-dependent build
# settings and rules.
#
# use : make PDDIR=/path/to/pure-data
#
# The following command will build the external and install the distributable 
# files into a subdirectory called build :
#
# make install PDDIR=/path/to/pure-data PDLIBDIR=./build

export
export cxx.flags

lib.name = vstplugin~

class.sources = src/vstplugin~.cpp src/vstsearch.cpp
vstplugin~.class.sources = ../vst/VSTPlugin.cpp ../vst/VST2Plugin.cpp

VST2DIR = ../vst/VST_SDK/VST2_SDK/
VST2_CFLAGS = -I"$(VST2DIR)/pluginterfaces/vst2.x/"
VST2_LIBS =
LOGLEVEL = 2

cflags = -Wno-unused -Wno-unused-parameter \
	-g \
	-std=c++14 \
    -I../vst \
	${VST2_CFLAGS} \
	-DLOGLEVEL=${LOGLEVEL} \
	$(empty)
ldlibs = $(VST2_LIBS)

define forWindows
  vstplugin~.class.sources += ../vst/VSTWindowWin32.cpp
  vstsearch.class.ldlibs += -L. -l:vstplugin~.$(extension)
  cflags += -DVSTTHREADS=1
  ldlibs += -lstdc++fs
  makefiles = probe.make
endef
define forLinux
  vstplugin~.class.sources += ../vst/VSTWindowX11.cpp
  vstsearch.class.ldlibs += -L. -l:vstplugin~.$(extension)
  cflags += -DTARGET_API_MAC_CARBON=1 -DDL_OPEN=1 -DUSE_X11=1 -DVSTTHREADS=1
  ldlibs += -L/usr/X11R6/lib -lX11
endef
define forDarwin
  common.sources += ../vst/VSTWindowCocoa.mm
  vstsearch.class.sources += ../vst/VSTPlugin.cpp ../vst/VST2Plugin.cpp 
  cflags += -fno-objc-arc -DVSTTHREADS=0
  ldlibs += -framework Cocoa
endef

# all extra files to be included in binary distribution of the library
datafiles = vstplugin~-help.pd ../README.md ../LICENSE.txt

include pd-lib-builder/Makefile.pdlibbuilder
