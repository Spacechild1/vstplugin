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
