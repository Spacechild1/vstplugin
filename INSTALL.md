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
