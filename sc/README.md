VSTPlugin SuperCollider extension
=================================

### Documentation:

See the various SC help files, in particular for the `VSTPlugin` and `VSTPluginController` classes.

---

### Known issues:

* The Supernova version of VSTPlugin only works on SuperCollider 3.11 and above!

* macOS: the VST GUI only works on SuperCollider 3.11 and above!
  Otherwise you get a warning if you try to open a plugin with "editor: true".

* Existing SynthDef files written with v0.4 or below don't work in v0.5, because the UGen structure has changed; you have to recreate them with the new version.

---

### Third-party clients:

If you want to port the VSTPlugin UGen to a third-party client,
have a look at the [UGen command reference](doc/VSTPlugin-UGen-Reference.md)
