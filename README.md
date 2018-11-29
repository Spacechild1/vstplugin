vstplugin~ 0.1-alpha
==============================================================================

*** WARNING: this is an alpha release, some things may change in the future ***

with vstplugin~ you can load VST plugins within Pd on Windows, MacOS and Linux.
it provides both a generic editor and a native window to show the plugin GUI.

features:
* load any kind of VST plugin (audio effect, MIDI effect, soft synth etc.)
* automate plugin programmatically with Pd messages
* can show either the native VST GUI (WIN32, Cocoa, X11) or a generic Pd editor
* preset management: read/write standard .fxp and .fxb files or
  set/get the plugin state as a Pd list to build your own preset management.
* choose between single and double precision (if the plugin supports it)
* MIDI input/output
* basic sequencing support (for arpeggiators, sequencers etc.)

NOTE: currently only VST2.x plugins are supported but VST3 support will come soon!
64bit VST plugins can only be loaded with the 64bit version of [vstplugin~] and vice versa.

see vstplugin~-help.pd for detailed instructions.

please report any issues or feature requests to https://git.iem.at/ressi/vstplugin/issues

==============================================================================

known issues:

on Windows and Linux, the native GUI window runs in its own thread, which means
that GUI updates shouldn't have a noticable effect on audio performance.
on MacOS, however, because of technical limitations the GUI must run on
the main thread - which happens to be the audio thread in Pd...
until we've found a better solution the user is adviced to keep native GUI
windows closed in low-latency realtime situations to avoid audio hick-ups.

to make the GUI work at all on MacOS we have to 'transform' Pd into a Cocoa app
and install an event polling routine, which is a bit adventurous to say the least.
for this reason, the default plugin editor on MacOS is the generic Pd editor.
you have to explicitly provide the flag "-gui" to get the native GUI.

==============================================================================

build and install:

vstplugin~ uses a slightly modified version of pd-lib-builder (https://github.com/pure-data/pd-lib-builder)
(to compile on Windows you need MinGW; it is recommended to use msys2: https://www.msys2.org/)

1) 	get the Steinberg VST2 SDK and copy it into /src.
	you should have a folder /src/VST_SDK/VST2_SDK/pluginterfaces/vst2.x
	with the header files aeffect.h, affectx.h and vstfxstore.
	(the VST2 SDK is officially deprecated by Steinberg and not easy to find. try .git-ci/get_vst.sh)

2) 	cd into /vstplugin and type 'make'. in case the Makefile doesn't automatically find your Pd installation,
	you can provide the path to Pd explicitly with:
	$ make PDDIR=...
	type 'make install' if you wan't to install the library. you can choose the installation directory with
	$ make install PDLIBDIR=...
