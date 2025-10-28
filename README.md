vstplugin v0.6.2
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

See [INSTALL.md](INSTALL.md) for build instructions.

See [pd/README.md](pd/README.md) resp. [sc/README.md](sc/README.md)
for more information about the Pd external resp. SC extensions.

Binaries are available here: https://git.iem.at/pd/vstplugin/-/releases.
The Pd external can also be installed with Deken (search for `vstplugin~`).

Please report any issues or feature requests to https://git.iem.at/pd/vstplugin/issues.

---

### Known issues:

* **ATTENTION macOS users**: the binaries are not signed/notarized, so you might have problems on macOS 10.15 (Catalina) and above.
  If you get security popups and the plugin refuses to load, please follow the steps in the section *macOS 10.15+* at the bottom!

* VST3 preset files created with vstplugin v0.3.0 or below couldn't be opened in other VST hosts and vice versa.
  This has been fixed in vstplugin v0.3.1. You can still open old "wrong" preset files, but this might go away in future versions; you're advised to open and save your old VST3 presets to "convert" them to the new format.

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
