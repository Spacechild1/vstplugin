vstplugin~ Pd external
======================

### Documentation:

See the help patch for the `[vstplugin~]` object.

---

### Known issues:

* macOS: because of technical limitations the GUI must run on the main thread - which happens to be the audio thread in Pd (at the time of writing)...
  This might get fixed in future Pd versions, but for now, macOS users are adviced to keep native GUI windows closed whenever possible to avoid audio drop-outs.

  There are two workarounds:

  a) run the plugin in a subprocess (see "-b" and "-p" options)

  b) use my Pd "eventloop" fork (source: https://github.com/Spacechild1/pure-data/tree/eventloop, binaries: https://github.com/Spacechild1/pure-data/releases, e.g. "Pd 0.51-1 event loop").
     NOTE: You have tick "Enable event loop" in the "Start up" settings.

* Linux: if you want to load plugins that are based on libpd (e.g. Camomile or Miller's pureVST) you need to compile `[vstplugin~]` yourself and configure CMake with `-DUSE_RTLD_DEEPBIND=ON`.
  This makes sure that the plugin will get the Pd API functions from the embedded libpd and not from the main Pd application; otherwise it would call API functions on the wrong Pd instance.
  However, RTLD_DEEPBIND seems to cause troubles with certain plugins, leading to obscure crashes in the C++ standard library. Using libpd-based VST plugins inside Pd is arguably an edge case,
  that's why USE_RTL_DEEPBIND is disabled by default.
