vstplugin v0.2.0-test
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


**NOTE:** 64bit VST plugins can only be loaded with the 64bit version of [vstplugin~] / VSTPlugin.scx and vice versa.

See the help files (vstplugin~-help.pd and VSTPlugin.schelp) for detailed instructions.

Please report any issues or feature requests to https://git.iem.at/pd/vstplugin/issues

---

### Known issues:

* On Windows and Linux, the native GUI window runs in its own thread, which means
that GUI updates shouldn't have a noticable effect on audio performance.
On MacOS, however, because of technical limitations the GUI must run on
the main thread[^1] - which happens to be the audio thread in Pd...
Until we've found a better solution, macOS users are adviced to keep native GUI
windows closed in low-latency realtime situations to avoid audio hick-ups.

* On SuperCollider, the VST GUI doesn't work (yet) on macOS, you get a warning if you try
to open a plugin with "editor: true".

* For VST3 plugins, the GUI is not available (yet).

* If you build a 32-bit(!) version 'VSTPlugin' with MinGW and Supercollider/Supernova has also been compiled with MinGW, exception handling might be broken due to a compiler bug.
This only seems to happen if either the plugin *or* the host link statically against libstdc++ and libgcc. By default, 'VSTPlugin' links statically, so we don't have to ship
additional DLLs and this generally works fine - unless you use a Supercollider version which was also built with MinGW but *dynamically* linked. In this case you should run cmake with
`-DSTATIC_LIBS=OFF` so that 'VSTPlugin' also links dynamically. To sum it up: MinGW <-> Visual Studio should always work, but MinGW (32-bit, dynamically linked) <-> MinGW (32-bit, statically linked) causes big troubles. Yes, it's ridiculous!

[^1]: to make the GUI work for Pd on macOS we have to 'transform' Pd into a Cocoa app
and install an event polling routine, which is a bit adventurous to say the least.

---

### Licensing:

The source code for Pd external and Supercollider UGen is permissively licensed, but note that you also have to comply with the licensing terms of the VST SDK you're using.

---

### Build instructions:

#### Prerequisites:

For VST2 support, get the Steinberg VST2 SDK and copy it into /vst.
You should have a folder vst/VST_SDK/VST2_SDK/pluginterfaces/vst2.x with the header files aeffect.h, affectx.h and vstfxstore.
(The VST2 SDK has been officially discontinued by Steinberg and is not easy to find. Try .git-ci/get_vst2.sh)

For VST3 support, get the Steinberg VST3 SDK and copy it into /vst.
Actually, you only need vst/VST_SDK/VST3_SDK/pluginterfaces/
(If you have git installed, run .git-ci/get_vst3.sh)

In case you already have the VST SDK(s) installed somewhere else on your system, you can set the path in the build system.

#### Pd:

vstplugin~ uses a *modified* version of pd-lib-builder (https://github.com/pure-data/pd-lib-builder)
(to compile on Windows you need MinGW; it is recommended to use Msys2: https://www.msys2.org/)

cd into pd/ and type 'make'. In case the Makefile doesn't automatically find your Pd installation, you can provide the path to Pd explicitly with `PDDIR`:
`$ make PDDIR=/path/to/pd`

Type 'make install' if you want to install the library. You can choose the installation directory with `PDLIBIDR`:
`$ make install PDLIBDIR=...`

The default setting is to build with both VST2 and VST3 support. If you only want to support a specific VST version, you can set the 'VST2' or 'VST3' variable in the make command. 
E.g. if you want to compile with only VST3 support, do this:
`$ make PDDIR=/path/to/pd VST2=0`

In case you have the SDK installed somewhere else, you can specify the location with the 'VST2DIR' and 'VST3DIR' variables.

#### SuperCollider:

In order to use the VSTPlugin.sc and VSTPluginController.sc classes, you need to first build the VSTPlugin UGen.
On macOS/Linux you can use GCC or Clang, on Windows you have to use VisualStudio because MinGW builds don't seem to work for some reason.

1) 	make sure you have CMake installed
2) 	get the SuperCollider source code (e.g. https://github.com/supercollider/supercollider)
3) 	cd into sc/ and create a build directory (e.g. build/)
4) 	*macOS/Linux:* cd into the build directory and do

	`cmake -DCMAKE_BUILD_TYPE=RELEASE -DCMAKE_INSTALL_PREFIX=/install/path -DSC_PATH=/path/to/supercollider ..`

	*Windows:* you have to tell CMake to generate a VisualStudio project (e.g. "Visual Studio 15 2017 Win64" for a 64 bit build) instead of a standard Unix makefile.
	It is recommended to use the cmake-gui GUI application to set the variables mentioned above and below.
	
	The default setting is to build with both VST2 and VST3 support. If you only want to support a specific version, you can set the 'VST2' and 'VST3' variables.
	E.g. if  you want to compile without VST2 support, run cmake with `-DVST2=OFF`.
	
	In case you have the SDK installed somewhere else, you can specify the location with the 'VST2DIR' and 'VST3DIR' variables.
	
	You can change `CMAKE_BUILD_TYPE` from `RELEASE` to `DEBUG` if you want a debug build.
	`CMAKE_INSTALL_PREFIX` would typically be your SuperCollider Extensions folder.
	`SC_PATH` must point to the folder containing the SuperCollider source code (with the subfolders common/ and include/).
	
	Set 'SUPERNOVA' to 'ON' if you want to build VSTPlugin for Supernova, but note that this doesn't work yet because of several bugs in Supernova (as of SC 3.10.3).
	However, this might be fixed in the next minor SC release.

5) 	*macOS/Linux:* type `make`

	*Windows:* open VSTPlugin.sln with Visual Studio and build the solution.

6)	*macOS/Linux:* type `make install` if you want to install

	Windows: build the project `INSTALL` if you want to install

---

### Final words

Now comes the mystery.