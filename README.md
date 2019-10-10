vstplugin v0.2.0
================

This project allows you to use VST plugins in Pd and SuperCollider on Windows, MacOS and Linux.
It includes a Pd external called "vstplugin~" and a SuperCollider UGen called "VSTPlugin" (+ various SC classes).

### Features:

* supports all VST2 and VST3 plugins (audio effect, MIDI effect, instruments, etc.)
* search and probe plugins in the standard VST directories or in user defined paths
* automate plugin parameters programmatically (sample accurate for VST3 plugins)
* arbitrary number of inputs/outputs + VST3 side chain inputs/outputs
* use either the native VST GUI (WIN32, Cocoa, X11) or a generic editor
* preset management: read/write standard VST preset files or
  set/get the plugin state as raw data to build your own preset management
* MIDI input/output
* basic sequencing support (for arpeggiators, sequencers etc.)


**NOTE:** 64bit VST plugins can only be loaded with the 64bit version of [vstplugin~] / VSTPlugin.scx and vice versa.

See the help files (vstplugin~-help.pd and VSTPlugin.schelp) for detailed instructions.

Please report any issues or feature requests to https://git.iem.at/pd/vstplugin/issues

---

### Known issues:

* On Windows and Linux, the native GUI window runs in a dedicated UI thread, which means
that GUI updates shouldn't have a noticable effect on audio performance.
On MacOS, however, because of technical limitations the GUI must run on
the main thread[^1] - which happens to be the audio thread in Pd...
Until we've found a better solution, macOS users are adviced to keep native GUI
windows closed in low-latency realtime situations to avoid audio hick-ups.

* On SuperCollider, the VST GUI doesn't work (yet) on macOS, you get a warning if you try
to open a plugin with "editor: true".

* If you build a 32-bit(!) version with MinGW and the host (Pd or Supercollider) has also been compiled with MinGW, exception handling might be broken due to a compiler bug.
This only seems to happen if either the plugin *or* the host link statically against libstdc++ and libgcc. By default we link statically, so we don't have to ship
additional DLLs. This generally works fine (because Pd is statically linked and Supercollider is nowadays built with MSVC), but it might cause troubles if you build a *dynamically* linked 32-bit Supercollider with MinGW.
In this special case you should run cmake with `-DSTATIC_LIBS=OFF` so that 'VSTPlugin' also links dynamically.
To sum it up: MinGW <-> Visual Studio should always work, but MinGW (32-bit, dynamically linked) <-> MinGW (32-bit, statically linked) causes big troubles. Yes, it's ridiculous!

[^1]: to make the GUI work for Pd on macOS we have to 'transform' Pd into a Cocoa app
and install an event polling routine, which is a bit adventurous to say the least.

---

### Licensing:

The source code for the Pd external and Supercollider UGen is permissively licensed, but note that you also have to comply with the licensing terms of the VST SDK(s) you're using!

---

### Build instructions:

This project is built with CMake, supported compilers are GCC, Clang and MSVC.
(On Windows, you can also compile with MinGW; it is recommended to use Msys2: https://www.msys2.org/)

By default, the project is built in release mode. You can change `CMAKE_BUILD_TYPE` from `RELEASE` to `DEBUG` if you want a debug build, for example.

If you only want to build either the Pd or Supercollider version, simply set the 'PD' or 'SC' variable to 'OFF'.

#### Prerequisites:

##### VST SDK:

For VST2 support, get the Steinberg VST2 SDK and copy it into /vst.
You should have a folder vst/VST_SDK/VST2_SDK/pluginterfaces/vst2.x with the header files aeffect.h and affectx.h.

The VST2 SDK has been officially discontinued by Steinberg. If you have a VST2 license but lost the files, you can get them with .git-ci/get_vst2.sh.
Otherwise you can try free alternatives like FST (https://git.iem.at/zmoelnig/FST.git - copy "fst.h" into vst/VST_SDK/VST2_SDK/pluginterfaces/vst2.x and set the 'FST' option to 'ON').
Use at your own risk!

For VST3 support, get the Steinberg VST3 SDK and copy it into /vst.
Actually, you only need vst/VST_SDK/VST3_SDK/pluginterfaces/
(If you have git installed, run .git-ci/get_vst3.sh)

The default setting is to build with both VST2 and VST3 support.
If you only want to support a specific version, you can set the 'VST2' and 'VST3' variables in the CMake project.
E.g. if  you want to compile without VST2 support, run cmake with `-DVST2=OFF`.

In case you already have the VST SDK(s) installed somewhere else on your system,
you can provide the path to CMake by setting the 'VST2DIR' and 'VST3DIR' variables.

##### Pd:

Make sure you have Pd installed somewhere. If Pd is not found automatically, you can set the paths manually with `-DPD_INCLUDEDIR="/path/to/Pd/src"`.
On Windows you would also need `-DPD_BINDIR="/path/to/Pd/bin"`; you can set both variables at the same time with `-DPD_DIR="/path/to/Pd"`.

By default, [vstplugin~] is installed in the standard externals directory, but you can override it with `-DPD_INSTALLDIR="/path/to/my/externals"`.

##### SuperCollider:

Get the SuperCollider source code (e.g. https://github.com/supercollider/supercollider).
`SC_INCLUDEDIR` must point to the folder containing the SuperCollider source code (with the subfolders *common/* and *include/*).

With `-DSC_INSTALLDIR="/path/to/my/extensions"` you can choose the installation directory, which would typically be your SuperCollider extensions folder.
	
Set 'SUPERNOVA' to 'ON' if you want to build VSTPlugin for Supernova, but note that this doesn't work yet because of several bugs in Supernova (as of SC 3.10.3).
However, this might be fixed in the next minor SC release.

#### Build:

1)	create a build directory, e.g. *build/*.
2)	cd into the build directory and run `cmake ..` + the necessary variables
	*or* set the variables in the cmake-gui and click "Configure" + "Generate"
3)	in the build directory type `make`

	*MSVC:* open VSTPlugin.sln with Visual Studio and build the solution.

4)	type `make install` to install

	*MSVC:* build the project `INSTALL` to install

---

### Final words

Now comes the mystery.
