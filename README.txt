==============================================================================
*** vstplugin~ ***
==============================================================================

with vstplugin~ you can load VST plugins within Pd on Windows, MacOS and Linux.
it provides both a generic editor and a native window to show the plugin GUI.

NOTE: currently only VST2.x plugins are supported but VST3 support will come soon!

see vstplugin~-help.pd for detailed instructions.

please report any issues or feature requests to https://git.iem.at/ressi/vstplugin/issues

==============================================================================

build and install:

vstplugin~ uses the pd-lib-builder build system (https://github.com/pure-data/pd-lib-builder)
(to compile on Windows you need MinGW. it is recommended to use msys2: https://www.msys2.org/)

1) 	get the Steinberg VST2 SDK and copy it into /src.
	you should have a folder /src/VST_SDK/VST2_SDK/pluginterfaces/vst2.x
	with the header files aeffect.h, affectx.h and vstfxstore.
	(the VST2 SDK is officially deprecated by Steinberg and not easy to find. try .git-ci/get_vst.sh)

2) 	cd into /vstplugin and type 'make'. in case the Makefile doesn't automatically find your Pd installation,
	you can provide the path to Pd explicitly with: 
	$ make PDDIR=...
	type 'make install' if you wan't to install the library. you can choose the installation directory with
	$ make install PDLIBDIR=...
