vstplugin v0.6.0-pre1
================

This project allows you to use VST plugins in Pd and SuperCollider on Windows, MacOS and Linux.
It includes a Pd external called "vstplugin~" and a SuperCollider UGen called "VSTPlugin" (+ various SC classes).

### Features:

* supports all VST2 and VST3 plugins (audio effect, MIDI effect, instruments, etc.)
* automate plugin parameters (sample accurate for VST3 plugins)
* arbitrary number of inputs/outputs + VST3 multi-bus support
* use either the native VST GUI (WIN32, Cocoa, X11) or a generic editor
* preset management: read/write standard VST preset files or
  set/get the plugin state as raw data to build your own preset management
* MIDI input/output
* transport and tempo support (for arpeggiators, sequencers, parameters with musical time units, etc.)
* offline rendering
* search for plugins in the standard VST directories or in user defined paths
* bit-bridging and sandboxing
* use Windows plugins on Linux (with Wine)
* (optional) multithreaded plugin processing

### Supported platforms:
* Windows (i386 and amd64)
* macOS (i386, amd64 and arm64)
* Linux (i386, amd64, arm and arm64)


See the help files (`vstplugin~-help.pd` and `VSTPlugin.schelp`) for detailed instructions.

Binaries are available here: https://git.iem.at/pd/vstplugin/-/releases. The Pd external can also be installed with Deken (search for `vstplugin~`).

Please report any issues or feature requests to https://git.iem.at/pd/vstplugin/issues.

---

### Known issues:

* **ATTENTION macOS users**: the binaries are not signed/notarized, so you might have problems on macOS 10.15 (Catalina) and above.
  If you get security popups and the plugin refuses to load, please follow the steps in the section *macOS 10.15+* at the bottom!

* The Supernova version of VSTPlugin only works on SuperCollider 3.11 and above!

* macOS/SuperCollider: the VST GUI only works on SuperCollider 3.11 and above!
  Otherwise you get a warning if you try to open a plugin with "editor: true".

* macOS/Pd: because of technical limitations the GUI must run on the main thread - which happens to be the audio thread in Pd (at the time of writing)...
  This might get fixed in future Pd versions, but for now, macOS users are adviced to keep native GUI windows closed whenever possible to avoid audio drop-outs.

  There are two workarounds:

  a) run the plugin in a subprocess (see "-b" and "-p" options)

  b) use my Pd "eventloop" fork (source: https://github.com/Spacechild1/pure-data/tree/eventloop, binaries: https://github.com/Spacechild1/pure-data/releases, e.g. "Pd 0.51-1 event loop").
     NOTE: You have tick "Enable event loop" in the "Start up" settings.

* VST3 preset files created with vstplugin v0.3.0 or below couldn't be opened in other VST hosts and vice versa.
  This has been fixed in vstplugin v0.3.1. You can still open old "wrong" preset files, but this might go away in future versions; you're advised to open and save your old VST3 presets to "convert" them to the new format.

* Existing SynthDef files written with v0.4 or below don't work in v0.5, because the UGen structure has changed; you have to recreate them with the new version.

---

### Bridging/sandboxing

Since v0.4 it is possible to run 32-bit VST plugins with a 64-bit version of Pd/Supercollider and vice versa.

*vstplugin* always prefers native plugins over bridged plugin with the same name. Plugins that have been "converted" with an external plugin bridge (e.g. jBridge, LinVst, Yabridge) will appear as native plugins.

#### Windows

Bit-bridging is usually used for running old 32-bit plugins - many of which will never see an update - on modern 64-bit host applications.

By default, vstplugin searches for plugins in both `%PROGRAMFILES%` and `%PROGRAMFILES(x86)%`.

#### macOS

On macOS, running 32-bit (Intel) plugins is only possible up to macOS 10.14 because macOS 10.15 eventually dropped 32-bit support.
However, bit-bridging is still useful for Apple M1 MacBooks because it allows to run existing 64-bit Intel VST plugins in a native ARM Pd or SC app.

#### Linux

On Linux, classic bit-bridging (= running 32-bit plugins on a 64-bit host) is not very common because plugins are either open source or they are recent enough to provide 64-bit versions.

However, since *vstplugin v0.5* it is also possible to run 64-bit and 32-bit Windows plugins on Linux! For this you need to have Wine installed on your system.
Unfortunately, there are several different Wine versions (stable, development, staging, etc.) and they are not 100% compatible.
The binaries available at https://git.iem.at/pd/vstplugin/-/releases are built against the standard Wine version shipped with Debian.
If you want to use a newer Wine version, you might have to build *vstplugin* from source.

*vstplugin* searches for plugins in the standard Windows VST directories inside the `drive_c` directory of your Wine folder.
The default Wine folder is `~/.wine` and the default Wine loader is the `wine` command; both can be overriden with the `WINEPREFIX` resp. `WINELOADER` environment variables.

NOTE: plugins "converted" by LinVst or Yabridge will be automatically preferred over the built-in Wine bridge. Later we might provide an option to change this.

WARNING: The Wine plugin bridge is still experimental and some plugins might not work as expected.
Please report any problems on the issue tracker (https://git.iem.at/pd/vstplugin/issues), so I can either fix or document them.

---

### Licensing:

The source code for the Pd external and Supercollider UGen is permissively licensed, but note that you also have to comply with the licensing terms of the VST SDK(s) you're using!

---

### Build instructions:

This project is built with CMake, supported compilers are GCC, Clang and MSVC.
(On Windows, you can also compile with MinGW; it is recommended to use Msys2: https://www.msys2.org/)

By default, the project is built in release mode. You can change `CMAKE_BUILD_TYPE` from `RELEASE` to `DEBUG` if you want a debug build, for example.

If you only want to only build the Pd or Supercollider version, simply set the `SC` resp. `PD` variable to `OFF`.

##### Static linking

If `STATIC_LIBS` is `ON`, the binaries are linked statically with `libstdc++` and `libgcc` (for MinGW also `libpthread`);
otherwise they are linked dynamically. The default is `ON` for MinGW (Windows) and `OFF` for Linux.
On other platforms (Visual Studio, macOS), the option has no effect and you always get a dynamically linked build.

Static linking helps if you want to share the binaries with other people because they might not have the required library versions installed on their system.
This is particularly true for Windows. On Linux, however, static linking can lead to symbol collisions under certain circumstances.

Dynamic linking is generally preferred for destributing binaries through system package managers like "apt".


#### Prerequisites:

#### VST SDK:

For VST2 support, get the Steinberg VST2 SDK and copy it into /vst.

You should have a folder `vst/VST_SDK/VST2_SDK/pluginterfaces/vst2.x` with the header files `aeffect.h` and `affectx.h`.

The VST2 SDK has been officially discontinued by Steinberg. If you have a VST2 license but lost the files, you can get them with `.git-ci/get_vst2.sh`.
Otherwise you can try free alternatives like *FST* (https://git.iem.at/zmoelnig/FST.git).

For VST3 support, get the Steinberg VST3 SDK and copy it into /vst.
You should have a folder `vst/VST_SDK/VST3_SDK/pluginterfaces`; you don't need the rest of the SDK.
(If you have git installed, you can easily install it with `./.git-ci/get_vst3.sh`)

The default setting is to build with both VST2 and VST3 support.
If you only want to support a specific version, you can set the `VST2` and `VST3` CMake variables.
E.g. if you want to compile without VST2 support, run cmake with `-DVST2=OFF`.

In case you already have the VST SDK(s) installed somewhere else on your system,
you can provide the path to CMake by setting the `VST2DIR` and `VST3DIR` variables.

Because earlier versions of the VST3 SDK also included the VST2 SDK headers,
the project will also look for the VST2 headers in `vst/VST_SDK/VST3_SDK/pluginterfaces/vst2.x`.

#### Pd:

Make sure you have Pd installed somewhere. If Pd is not found automatically, you have to do the following:

* Linux and macOS: set `PD_INCLUDEDIR` to the directory containing `m_pd.h`;

* Windows: set `PD_DIR` to your Pd directory (with the subfolders *src/* and *bin/*); this will automatically set `PD_INCLUDEDIR` and `PD_BINDIR`.

By default, *vstplugin~* is installed to the standard externals directory, but you can change it by overriding `PD_INSTALLDIR`.

If you don't want to build the Pd external, set `PD` to `OFF`.

#### SuperCollider:

Get the SuperCollider source code (e.g. https://github.com/supercollider/supercollider).
`SC_INCLUDEDIR` must point to the folder containing the SuperCollider source code (with the subfolders *common/* and *include/*).

By default, *VSTPlugin* is installed to the standard SuperCollider extensions folder, but you can change it by overriding `SC_INSTALLDIR`.

Set `SUPERNOVA` to `ON` if you want to build VSTPlugin for Supernova.

If you don't want to build the SuperCollider extension, set `SC` to `OFF`.


#### Windows

If you want to enable bit bridging (running 32-bit plugins on a 64-bit host and vice versa), you have to perform the following steps:

1) build and install the project with a 64-bit compiler (e.g. in a `build64` folder)

2) build and install the project with a 32-bit compiler (e.g. in a `build32` folder)

3) 64-bit: set the `HOST32_PATH` variable to the path of the 32-bit(!) host.exe and reinstall the project.

4) 32-bit: set the `HOST_AMD64_PATH` variable to the path of the 64-bit(!) host.exe and reinstall the project.

By default, the minimum Windows deployment target is Windows 7. You may choose a *higher* version by setting the `WINVER` CMake variable.

If you compile with MinGW, make sure to choose the appropriate generator with `cmake .. -G "Unix Makefiles"`.
Alternatively, you can pick a generator in cmake-gui when you first click "Configure". In this case you should also select the correct toolchain.

##### Warning about 32-bit MinGW

If you build a 32-bit(!) version with MinGW and the host (Pd or Supercollider) has also been compiled with MinGW, exception handling might be broken due to a compiler bug.
This only seems to happen if either the plugin or the host (but not both!) link statically against libstdc++ and libgcc. By default we link statically, so we don't have to ship additional DLLs.
This generally works fine (because Pd is statically linked and Supercollider is nowadays built with MSVC), but it might cause troubles if you build a *dynamically* linked 32-bit Supercollider/Pd with MinGW.
In this special case you should set `STATIC_LIBS` to `OFF` so that `VSTPlugin` resp. `vstplugin~` are also linked dynamically.
To sum it up: MinGW <-> Visual Studio should always work, but MinGW (32-bit, dynamically linked) <-> MinGW (32-bit, statically linked) causes big troubles.
I know, it's ridiculous!


#### macOS

You can build a universal binary with `-DCMAKE_OSX_ARCHITECTURES=<archs>`.
As a side effect, this will also enable bit-bridging between the specified architectures.

For example, `-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"` will build for Intel and ARM.
On ARM machines in particular, this would allow you to use existing Intel plugins in ARM versions of Pd/SC.

Alternatively, you can build individual host apps:

* Intel: You can build a 32-bit host application (for running old 32-bit plugins) by setting `BUILD_HOST32` to `ON`.
  Note that the macOS 10.14 SDK dropped support for compiling 32-bit applications; you must use Xcode 9.4 or earlier.

* ARM: You can build a 64-bit Intel host application (for running existing Intel plugins) by setting `BUILD_HOST_AMD64` to `ON`.

By default, the minimum macOS deployment target is OSX 10.9. You may choose a *higher* version by setting the `CMAKE_OSX_DEPLOYMENT_TARGET` CMake variable.


#### Linux

Dependencies: `libx11-dev`

You can build a 32-bit host application (for running old 32-bit plugins) by setting `BUILD_HOST32` to `ON`.
Make sure to install the relevant 32-bit toolchain and libraries:
```
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install libx11-dev:i386 gcc-multilib g++-multilib
```


#### Build project:

1)	create a build directory, e.g. "build", next to the topmost "CMakeLists.txt"

2)	cd into the build directory and run `cmake ..` + the necessary variables

    *or* set the variables in cmake-gui and click "Configure" + "Generate"

3)	build with `cmake --build . -j -v`

4)	install with `cmake --build . -v -t install`


#### Build Wine host:

To enable Wine support on Linux, you need to follow these steps:

1)  For 64-bit Wine, install `wine64-tools` or `wine-[branch]-dev` (depending on the Wine distro);

    for 32-bit Wine, follow the steps for building the 32-bit host on Linux and then install `wine32-tools` or `wine-[branch]-dev`.

2)  Create another build directory, e.g. `build_wine`, and `cd` into it.

3)  Set `BUILD_WINE` to `ON`.
   `PD_INSTALLDIR` and `SC_INSTALLDIR` should be the same as for the regular build.
    If you don't need the Pd external or SuperCollider extension, set `PD` resp. `SC` to `OFF`.

4)  Build + install the project with `cmake --build . -j -v -t install`;
    this will install `host_pe_amd64` (and optionally `host_pe_i386`) in the specified directories.


### macOS 10.15+

Please follow these steps (many thanks to Joseph Anderson) after downloading and installing:

1)  un-quarantine VSTPlugin/vstplugin~ executables:

    Using the terminal, navigate to your Pd external resp. SC extension folder and then run:

    SC: `xattr -rd com.apple.quarantine ./VSTPlugin`

    Pd: `xattr -rd com.apple.quarantine ./vstplugin~`

2)  add unsigned VST plugins to Gatekeeper's enabled list:

    Using the terminal, navigate to the folder(s) containing VSTs to enable.
    The following will create a label, ApprovedVSTs, and then add all VSTs in the directory:

    `spctl --add --label "ApprovedVSTs" *.vst *.vst3`

    Once this is done, the following informs Gatekeeper these are approved:

    `spctl --enable --label "ApprovedVSTs"`

3)  clear the plugin cache

    It is a good idea to go ahead and clear the plugin cache, in case some quarantined plugins have been black-listed already.

    SC: boot the SuperCollider Server, then evaluate: `VSTPlugin.clear`

    PD: open `vstplugin~-help.pd`, visit `[pd search]` and click the `[clear 1(` message.
