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

VST2DIR = ../vst/VST_SDK/VST2_SDK/
VST2_CFLAGS = -I"$(VST2DIR)/pluginterfaces/vst2.x/"
VST2_LIBS =
LOGLEVEL = 2
PROBE = probe
PROBE_EXE = ${PROBE}
PROBE_SRC = ../vst/${PROBE}.cpp
PROBE_DEPS =
PROBE_LDLIBS =

cflags = \
	-g \
	-std=c++14 \
    -I../vst \
	${VST2_CFLAGS} \
	-DLOGLEVEL=${LOGLEVEL} \
	$(empty)
ldlibs = $(VST2_LIBS)

suppress-wunused = 1

makefiles = ${PROBE}.make

# on Windows (MinGW), vstsearch and probe are just stubs which dynamically link to vstplugin~.dll
# (dramatically reduces binary size: ~20kB vs ~1MB)
define forWindows
  vstplugin~.class.sources += ../vst/VSTPlugin.cpp ../vst/VST2Plugin.cpp ../vst/VSTWindowWin32.cpp
  vstsearch.class.ldlibs += -L. -l:vstplugin~.$(extension)
  PROBE_EXE = ${PROBE_EXE}.exe
  PROBE_DEPS += vstplugin~.$(extension)
  cflags += -DVSTTHREADS=1
  ldlibs += -lstdc++fs
  # -municode for wmain, -mwindows to hide console
  PROBE_LDLIBS += -static-libgcc -static-libstdc++ -municode -mwindows -L. -l:vstplugin~.$(extension)
endef
define forLinux
  common.sources += ../vst/VSTPlugin.cpp ../vst/VST2Plugin.cpp ../vst/VSTWindowX11.cpp
  PROBE_SRC += ../vst/VSTPlugin.cpp ../vst/VST2Plugin.cpp ../vst/VSTWindowX11.cpp
  cflags += -DTARGET_API_MAC_CARBON=1 -DDL_OPEN=1 -DUSE_X11=1 -DVSTTHREADS=1
  ldlibs += -L/usr/X11R6/lib -lX11 -ldl
  PROBE_LDLIBS += -L/usr/X11R6/lib -lX11 -ldl
endef
define forDarwin
  common.sources += ../vst/VSTPlugin.cpp ../vst/VST2Plugin.cpp ../vst/VSTWindowCocoa.mm
  PROBE_SRC += ../vst/VSTPlugin.cpp ../vst/VST2Plugin.cpp ../vst/VSTWindowCocoa.mm
  cflags += -fno-objc-arc -DVSTTHREADS=0
  ldlibs += -framework Cocoa
  PROBE_LDLIBS += -framework Cocoa
endef

# all extra files to be included in binary distribution of the library
datafiles = vstplugin~-help.pd ../README.md ../LICENSE.txt

include pd-lib-builder/Makefile.pdlibbuilder
