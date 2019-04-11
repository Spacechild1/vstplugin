vstplugin v0.1.2
================

This project allows you to use VST plugins in Pd and SuperCollider on Windows, MacOS and Linux.
It includes a Pd external called "vstplugin~" and a SuperCollider UGen called "VSTPlugin" (+ various SC classes).

### Features:

* use any VST plugin (audio effect, MIDI effect, soft synth etc.)
* search and probe plugins in the standard VST directories or in user defined paths
* automate plugin parameters programmatically
* use either the native VST GUI (WIN32, Cocoa, X11) or a generic editor
  (NOTE: the VST GUI doesn't work [yet] for SuperCollider on macOS)
* preset management: read/write standard .fxp and .fxb files or
  set/get the plugin state as raw data to build your own preset management
* MIDI input/output
* basic sequencing support (for arpeggiators, sequencers etc.)


**NOTE:** currently only VST2.x plugins are supported but VST3 support will come soon!
64bit VST plugins can only be loaded with the 64bit version of [vstplugin~] / VSTPlugin.scx and vice versa.

See the help files (vstplugin~-help.pd and VSTPlugin.schelp) for detailed instructions.

Please report any issues or feature requests to https://git.iem.at/pd/vstplugin/issues

---

### Known issues:

On Windows and Linux, the native GUI window runs in its own thread, which means
that GUI updates shouldn't have a noticable effect on audio performance.

On MacOS, however, because of technical limitations the GUI must run on
the main thread *)- which happens to be the audio thread in Pd...
Until we've found a better solution, macOS users are adviced to keep native GUI
windows closed in low-latency realtime situations to avoid audio hick-ups.
For this reason, the default GUI on MacOS is the generic Pd editor.
You have to explicitly provide the "-vstgui" flag to get the VST GUI.

On SuperCollider, the VST GUI doesn't work (yet) on macOS, you get a warning if you try
to open a plugin with "editor: true".

*) to make the GUI work for Pd on macOS we have to 'transform' Pd into a Cocoa app
and install an event polling routine, which is a bit adventurous to say the least.

---

### Build instructions:

#### Pd:

vstplugin~ uses a *modified* version of pd-lib-builder (https://github.com/pure-data/pd-lib-builder)
(to compile on Windows you need MinGW; it is recommended to use Msys2: https://www.msys2.org/)

1) 	get the Steinberg VST2 SDK and copy it into /vst.
	You should have a folder vst/VST_SDK/VST2_SDK/pluginterfaces/vst2.x	with the header files aeffect.h, affectx.h and vstfxstore.
	(the VST2 SDK is officially deprecated by Steinberg and not easy to find. Try .git-ci/get_vst.sh)

2) 	cd into pd/ and type 'make'. In case the Makefile doesn't automatically find your Pd installation, you can provide the path to Pd explicitly with `PDDIR`:
	`$ make PDDIR=/path/to/pd`
	Type 'make install' if you want to install the library. You can choose the installation directory with `PDLIBIDR`:
	`$ make install PDLIBDIR=...`
	
#### SuperCollider:

In order to use the VSTPlugin.sc and VSTPluginController.sc classes, you need to first build the VSTPlugin UGen.
On macOS/Linux you can use GCC or Clang, on Windows you have to use VisualStudio because MinGW builds don't seem to work for some reason.

1) 	make sure you have CMake installed
2) 	get the Steinberg VST2 SDK (same as with Pd)
    You should have a folder vst/VST_SDK/VST2_SDK/pluginterfaces/vst2.x	with the header files aeffect.h, affectx.h and vstfxstore.
	(the VST2 SDK is officially deprecated by Steinberg and not easy to find. Try .git-ci/get_vst.sh)
3) 	get the SuperCollider source code (e.g. https://github.com/supercollider/supercollider)
4) 	cd into sc/ and create a build directory (e.g. build/)
5) 	*macOS/Linux:* cd into the build directory and do

	`cmake -DCMAKE_BUILD_TYPE=RELEASE -DCMAKE_INSTALL_PREFIX=/install/path -DSC_PATH=/path/to/supercollider ..`
	
	You can change `CMAKE_BUILD_TYPE` from `RELEASE` to `DEBUG` if you want a debug build. 
	`CMAKE_INSTALL_PREFIX` would typically be your SuperCollider Extensions folder.
	`SC_PATH` must point to the folder containing the SuperCollider source code (with the subfolders common/ and include/).
	
	*Windows:* you have to tell CMake to generate a VisualStudio project (e.g. "Visual Studio 15 2017 Win64" for a 64 bit build) instead of a standard Unix makefile.
	It is recommended to use the cmake-gui GUI application to set the variables mentioned above.

6) 	*macOS/Linux:* type `make`
    
    *Windows:* open VSTPlugin.sln with Visual Studio and build the solution.
    
7)	*macOS/Linux:* type `make install` if you want to install
	
	Windows: build the project `INSTALL` if you want to install
