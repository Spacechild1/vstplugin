vstplugin v0.4.1
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
* bit bridging and sandboxing
* multithreading

**NOTE:** It is now possible to load 32-bit VST plugins with a 64-bit version of [vstplugin~] / VSTPlugin.scx and vice versa,
but the required bit bridging is experimental and incurs some CPU overhead.

See the help files (vstplugin~-help.pd and VSTPlugin.schelp) for detailed instructions.

Please report any issues or feature requests to https://git.iem.at/pd/vstplugin/issues

---

### Known issues:

* The Supernova version of VSTPlugin only works on SuperCollider 3.11 and above (not released yet at the time of writing).

* macOS/SuperCollider: the VST GUI only works on SuperCollider 3.11 and above. Otherwise you get a warning if you try to open a plugin with "editor: true".

* macOS/Pd: because of technical limitations the GUI must run on the main thread - which happens to be the audio thread in Pd (at the time of writing)... This might get fixed in future Pd versions, but for now, macOS users are adviced to keep native GUI windows closed whenever possible to avoid audio drop-outs.

 There are two options work around this issue:

 a) run the plugin in a subprocess (see "-b" and "-p" options)

 b) use my Pd "eventloop" fork (source: https://github.com/Spacechild1/pure-data/tree/eventloop; binaries: https://github.com/Spacechild1/pure-data/releases, e.g. "Pd 0.51-1 event loop"). NOTE: You have tick "Enable event loop" in the "Start up" settings.

* The macOS binaries are *unsigned*, so you have to workaround the macOS Gatekeeper. See the section "macOS 10.15+" for more information.

* VST3 preset files created with vstplugin v0.3.0 or below couldn't be opened in other VST hosts and vice versa because of a mistake in the (de)serialization of VST3 plugin IDs. This has been fixed in vstplugin v0.3.1. You can still open old "wrong" preset files, but this might go away in future versions, so you're advised to open and save your old VST3 presets to "convert" them to the new format. But first make sure to clear the plugin cache and do a new search to update the plugin IDs.

* If you build a 32-bit(!) version with MinGW and the host (Pd or Supercollider) has also been compiled with MinGW, exception handling might be broken due to a compiler bug.
This only seems to happen if either the plugin or the host (but not both!) link statically against libstdc++ and libgcc. By default we link statically, so we don't have to ship
additional DLLs. This generally works fine (because Pd is statically linked and Supercollider is nowadays built with MSVC), but it might cause troubles if you build a *dynamically* linked 32-bit Supercollider/Pd with MinGW.
In this special case you should run cmake with `-DSTATIC_LIBS=OFF` so that VSTPlugin/vstplugin~ also link dynamically.
To sum it up: MinGW <-> Visual Studio should always work, but MinGW (32-bit, dynamically linked) <-> MinGW (32-bit, statically linked) causes big troubles. Yes, it's ridiculous!

---

### Licensing:

The source code for the Pd external and Supercollider UGen is permissively licensed, but note that you also have to comply with the licensing terms of the VST SDK(s) you're using!

---

### Build instructions:

This project is built with CMake, supported compilers are GCC, Clang and MSVC.
(On Windows, you can also compile with MinGW; it is recommended to use Msys2: https://www.msys2.org/)

By default, the project is built in release mode. You can change `CMAKE_BUILD_TYPE` from `RELEASE` to `DEBUG` if you want a debug build, for example.

If you only want to only build the Pd or Supercollider version, simply set the `SC` resp. `PD` variable to `OFF`.

When compiling with GCC on Linux or MinGW, we offer the option `STATIC_LIBS` to link statically with libstd++ and libgcc; the default is `ON`.

Static linking helps if you want to share the binaries with other people because they might not have the required library versions installed on their system.
Dynamic linking, on the other hand, is preferred for destributing via system package managers like "apt".

#### Prerequisites:

##### VST SDK:

For VST2 support, get the Steinberg VST2 SDK and copy it into /vst.
You should have a folder vst/VST_SDK/VST2_SDK/pluginterfaces/vst2.x with the header files aeffect.h and affectx.h.

The VST2 SDK has been officially discontinued by Steinberg. If you have a VST2 license but lost the files, you can get them with .git-ci/get_vst2.sh.
Otherwise you can try free alternatives like FST (https://git.iem.at/zmoelnig/FST.git - copy "fst.h" into vst/VST_SDK/VST2_SDK/pluginterfaces/vst2.x and set the 'FST' option to 'ON').
Use at your own risk!

For VST3 support, get the Steinberg VST3 SDK and copy it into /vst.
Actually, you only need vst/VST_SDK/VST3_SDK/pluginterfaces/
(If you have git installed, run ./.git-ci/get_vst3.sh)

The default setting is to build with both VST2 and VST3 support.
If you only want to support a specific version, you can set the `VST2` and `VST3` variables in the CMake project.
E.g. if  you want to compile without VST2 support, run cmake with `-DVST2=OFF`.

In case you already have the VST SDK(s) installed somewhere else on your system,
you can provide the path to CMake by setting the `VST2DIR` and `VST3DIR` variables.

##### Pd:

Make sure you have Pd installed somewhere. If Pd is not found automatically, you can set the paths manually with `-DPD_INCLUDEDIR="/path/to/Pd/src"`.
On Windows you would also need `-DPD_BINDIR="/path/to/Pd/bin"`; you can set both variables at the same time with `-DPD_DIR="/path/to/Pd"`.

By default, [vstplugin~] is installed in the standard externals directory, but you can override it with `-DPD_INSTALLDIR="/path/to/my/externals"`.

##### SuperCollider:

Get the SuperCollider source code (e.g. https://github.com/supercollider/supercollider).
`SC_INCLUDEDIR` must point to the folder containing the SuperCollider source code (with the subfolders *common/* and *include/*).

With `-DSC_INSTALLDIR="/path/to/my/extensions"` you can choose the installation directory, which would typically be your SuperCollider extensions folder.

Set `SUPERNOVA` to `ON` if you want to build VSTPlugin for Supernova, but note that this doesn't work yet because of several bugs in Supernova (as of SC 3.10.3).
However, this might be fixed in the next minor SC release.

#### Build:

1)	create a build directory, e.g. *build/*.
2)	cd into the build directory and run `cmake ..` + the necessary variables
    *or* set the variables in the cmake-gui and click "Configure" + "Generate"
3)	in the build directory type `make`

    *MSVC:* open vstplugin.sln with Visual Studio and build the solution.

4)	type `make install` to install

    *MSVC:* build the project `INSTALL` to install

#### macOS 10.15+

How to workaround macOS GateKeeper (many thanks to Joseph Anderson):

1) un-quarantine VSTPlugin/vstplugin~ executables

Using the terminal, navigate to the folder containing the .scx/.pd_darwin file and then run:

SC: `xattr -rd com.apple.quarantine *.scx host*`

Pd: `xattr -rd com.apple.quarantine *.pd_darwin host*`

2) add unsigned VSTs to Gatekeeper's enabled list

Using the terminal, navigate to the folder(s) containing VSTs to enable. The following will create a label, ApprovedVSTs, and then add all VSTs in the directory:

`spctl --add --label "ApprovedVSTs" *.vst`

Once this is done, the following informs Gatekeeper these are approved:

`spctl --enable --label "ApprovedVSTs"`

3) clear the plugin cache

It is a good idea to go ahead and clear the plugin cache, in case some quarantined plugins have been black-listed already.

SC: boot the SuperCollider Server, then evaluate: `VSTPlugin.clear`

PD: open `vstplugin~-help.pd`, visit `[pd search]` and click the `[clear 1(` message.

---

### Final words

Now comes the mystery.
