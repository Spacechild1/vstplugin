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

ifeq ($(wildcard $(VST2DIR)),)
# no VST2 headers, assume FST

ifneq ($(FST),)
# only add $(FST) to includes if not empty
fst_includes = -I"$(FST)"
endif
VST2_CFLAGS = -DUSE_FST=1 $(fst_includes)
VST2_LIBS =
else
# VST2 SDK found, use it!
VST2_CFLAGS = -I"$(VST2DIR)/pluginterfaces/vst2.x/"
VST2_LIBS =
endif

LOGLEVEL = 2
VST_SRC = ../vst/VSTPlugin.cpp ../vst/VST2Plugin.cpp
PROBE = probe
PROBE_EXE = $(PROBE)
PROBE_SRC = ../vst/$(PROBE).cpp $(VST_SRC)
PROBE_DEPS =
PROBE_LDLIBS =

shared.sources = src/vstplugin.cpp $(VST_SRC)

cflags = \
	-g \
	-std=c++14 \
    -I../vst \
	$(VST2_CFLAGS) \
	-DLOGLEVEL=$(LOGLEVEL) \
	$(empty)
ldlibs = $(VST2_LIBS)

suppress-wunused = 1

makefiles = $(PROBE).make

# vstplugin~ and vstsearch.dll both link to a shared library.
# we don't do this for probe.exe to make it work independently from Pd
define forWindows
  shared.sources += ../vst/VSTWindowWin32.cpp
  shared.ldlibs += -lstdc++fs
  PROBE_EXE = $(PROBE_EXE).exe
  PROBE_SRC += ../vst/VSTWindowWin32.cpp
  cflags += -DVSTTHREADS=1
  # -municode for wmain, -mwindows to hide console
  PROBE_LDLIBS += -lstdc++fs -static-libgcc -static-libstdc++ -municode -mwindows -static -lpthread
endef
define forLinux
  shared.sources += ../vst/VSTWindowX11.cpp
  PROBE_SRC += ../vst/VSTWindowX11.cpp
  cflags += -DTARGET_API_MAC_CARBON=1 -DDL_OPEN=1 -DUSE_X11=1 -DVSTTHREADS=1
  ldlibs += -L/usr/X11R6/lib -lX11 -ldl
  PROBE_LDLIBS += -L/usr/X11R6/lib -lX11 -ldl
endef
define forDarwin
  shared.sources += ../vst/VSTWindowCocoa.mm
  PROBE_SRC += ../vst/VSTWindowCocoa.mm
  cflags += -fno-objc-arc -DVSTTHREADS=0
  ldlibs += -framework Cocoa
  PROBE_LDLIBS += -framework Cocoa
endef

# all extra files to be included in binary distribution of the library
datafiles = vstplugin~-help.pd vstpluginbrowser.pd vstpluginbrowser-help.pd ../README.md ../LICENSE.txt

include pd-lib-builder/Makefile.pdlibbuilder
