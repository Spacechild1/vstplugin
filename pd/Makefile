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
VST3DIR = ../vst/VST_SDK/VST3_SDK/
VST_SRC = ../vst/VSTPlugin.cpp
VST_LIBS =
VST_CFLAGS =

# VST2 (default: on)
ifneq ($(VST2), 0)
VST_CFLAGS += -DUSE_VST2=1
VST_SRC += ../vst/VST2Plugin.cpp
ifeq ($(wildcard $(VST2DIR)),)
# no VST2 headers, assume FST
ifneq ($(FST),)
# only add $(FST) to includes if not empty
fst_includes = -I"$(FST)"
endif
VST_CFLAGS += -DUSE_FST=1 -I"$(fst_includes)"
else
# VST2 SDK found, use it!
VST_CFLAGS += -I"$(VST2DIR)/pluginterfaces/vst2.x/"
endif
endif

# VST3 (default: off)
ifeq ($(VST3), 0)
VST_SRC += ../vst/VST3Plugin.cpp
VST_CFLAGS += \
	-DUSE_VST3=1 \
	-I"$(VST3DIR)" \
	-I"$(VST3DIR)/pluginterfaces" \
	-I"$(VST3DIR)/pluginterfaces/base" \
	-I"$(VST3DIR)/pluginterfaces/vst" \
	-I"$(VST3DIR)/pluginterfaces/gui"
endif

LOGLEVEL = 2
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
	$(VST_CFLAGS) \
	-DLOGLEVEL=$(LOGLEVEL) \
	$(empty)

suppress-wunused = 1

makefiles = $(PROBE).make

# vstplugin~ and vstsearch.dll both link to a shared library.
# we don't do this for probe.exe to make it work independently from Pd
define forWindows
  shared.sources += ../vst/VSTWindowWin32.cpp
  shared.ldlibs += $(VST_LIBS) -lstdc++fs
  PROBE_EXE = $(PROBE_EXE).exe
  PROBE_SRC += ../vst/VSTWindowWin32.cpp
  cflags += -DVSTTHREADS=1
  # -municode for wmain, -mwindows to hide console
  PROBE_LDLIBS += $(VST_LIBS) -lstdc++fs -static-libgcc -static-libstdc++ -municode -mwindows -static -lpthread
endef
define forLinux
  shared.sources += ../vst/VSTWindowX11.cpp
  PROBE_SRC += ../vst/VSTWindowX11.cpp
  cflags += -DTARGET_API_MAC_CARBON=1 -DDL_OPEN=1 -DUSE_X11=1 -DVSTTHREADS=1
  ldlibs += $(VST_LIBS) -L/usr/X11R6/lib -lX11 -ldl
  PROBE_LDLIBS += $(VST_LIBS) -L/usr/X11R6/lib -lX11 -ldl
endef
define forDarwin
  shared.sources += ../vst/VSTWindowCocoa.mm
  PROBE_SRC += ../vst/VSTWindowCocoa.mm
  cflags += -fno-objc-arc -DVSTTHREADS=0
  ldlibs += $(VST_LIBS) -framework Cocoa
  PROBE_LDLIBS += $(VST_LIBS) -framework Cocoa
endef

# all extra files to be included in binary distribution of the library
datafiles = vstplugin~-help.pd vstpluginbrowser.pd vstpluginbrowser-help.pd ../README.md ../LICENSE.txt

include pd-lib-builder/Makefile.pdlibbuilder
