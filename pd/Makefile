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

class.sources = src/vstplugin~.cpp

VST2DIR = ../vst/VST_SDK/VST2_SDK/
VST3DIR = ../vst/VST_SDK/VST3_SDK/
VST_SRC = ../vst/Plugin.cpp
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

# VST3 (default: on)
ifneq ($(VST3), 0)
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
PROBE_SRC = ../vst/$(PROBE).cpp
PROBE_DEPS =
PROBE_LDLIBS =

cflags = \
	-std=c++14 \
	-I../vst \
	$(VST_CFLAGS) \
	-DHAVE_NRT_THREAD=0 \
	-DLOGLEVEL=$(LOGLEVEL) \
	$(empty)

suppress-wunused = 1

warn.flags = -Wno-format-security

makefiles = $(PROBE).make

define forWindows
  # common
  VST_SRC += ../vst/WindowWin32.cpp
  VST_LIBS += -lstdc++fs -static -lpthread
  cflags += -DHAVE_UI_THREAD=1
  # vstplugin~
  vstplugin~.class.sources += $$(VST_SRC)
  ldlibs += $$(VST_LIBS)
  # probe.exe
  PROBE_SRC += $$(VST_SRC)
  # -municode for wmain, -mwindows to hide console
  PROBE_LDLIBS += $$(VST_LIBS) -static-libgcc -static-libstdc++ -municode -mwindows
endef
define forLinux
  # common
  VST_SRC += ../vst/WindowX11.cpp
  VST_LIBS += -L/usr/X11R6/lib -lX11 -ldl
  cflags += -DTARGET_API_MAC_CARBON=1 -DDL_OPEN=1 -DUSE_X11=1 -DHAVE_UI_THREAD=1 -pthread
  # vstplugin~
  vstplugin~.class.sources += $$(VST_SRC)
  ldlibs += $$(VST_LIBS)
  # probe
  PROBE_SRC += $$(VST_SRC)
  PROBE_LDLIBS += $$(VST_LIBS)
endef
define forDarwin
  # common
  VST_SRC += ../vst/WindowCocoa.mm
  VST_LIBS += -framework Cocoa
  cflags += -fno-objc-arc -DHAVE_UI_THREAD=0
  # vstplugin~
  vstplugin~.class.sources += $$(VST_SRC)
  ldlibs += $$(VST_LIBS) 
  # probe
  PROBE_SRC += $$(VST_SRC)
  PROBE_LDLIBS += $$(VST_LIBS)
endef

# all extra files to be included in binary distribution of the library
datafiles = vstplugin~-help.pd vstpluginbrowser.pd vstpluginbrowser-help.pd ../README.md ../LICENSE.txt

include pd-lib-builder/Makefile.pdlibbuilder
