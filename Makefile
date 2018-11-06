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

common.sources = src/VSTPlugin.cpp src/VST2Plugin.cpp src/VSTWindow.cpp

VST2DIR = src/VST_SDK/VST2_SDK/pluginterfaces/vst2.x/

cflags = -Wno-unused -Wno-unused-parameter \
	-std=c++11 \
	-g \
	-I"${VST2DIR}" \
	$(empty)

define forWindows
  common.sources += src/VSTWindowWin32.cpp
endef
define forLinux
  cflags += -DTARGET_API_MAC_CARBON=1 -DDL_OPEN=1
endef


# all extra files to be included in binary distribution of the library
datafiles = 

include pd-lib-builder/Makefile.pdlibbuilder
