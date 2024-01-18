VSTPluginController {
	// class var
	const oscPacketSize = 1600; // safe max. OSC packet size
	// public
	var <>synth;
	var <>synthIndex;
	var <>info;
	var <>wait;
	var <midi;
	var <program;
	var <latency;
	// callbacks
	var <>parameterAutomated;
	var <>midiReceived;
	var <>sysexReceived;
	var <>latencyChanged;
	var <>pluginCrashed;
	// for dependants
	var <parameterCache;
	var <programCache;
	// private
	var oscFuncs;
	var currentPreset; // the current preset
	var window; // do we have a VST editor?
	var loading; // are we currently loading a plugin?
	var browser; // handle to currently opened browser
	var needQueryParams;
	var needQueryPrograms;
	var deferred; // deferred processing?

	*initClass {
		Class.initClassTree(Event);
		// custom event type for playing VSTis:
		Event.addEventType(\vst_midi, #{ arg server;
			var freqs, lag, offset, strumOffset, dur, sustain, strum;
			var bndl, noteoffs, vst, hasGate, midicmd, sel;

			freqs = ~freq = ~detunedFreq.value;

			~amp = ~amp.value;
			~midinote = (freqs.cpsmidi); // keep as float!
			strum = ~strum;
			lag = ~lag;
			offset = ~timingOffset;
			sustain = ~sustain = ~sustain.value;
			vst = ~vst.value.midi;
			hasGate = ~hasGate ? true;
			midicmd = ~midicmd;
			sel = (midicmd ++ "Msg").asSymbol;
			// NB: asControlInput resolves unwanted Rests in arrays, otherwise sendMidiMsg()
			// would throw an error when trying to build the Int8Array.
			bndl = ~midiEventFunctions[midicmd].valueEnvir.asCollection.asControlInput.flop;
			bndl = bndl.collect({ arg args;
				vst.performList(sel, args);
			});

			if (strum == 0) {
				~schedBundleArray.(lag, offset, server, bndl, ~latency);
			} {
				if (strum < 0) { bndl = bndl.reverse };
				strumOffset = offset + Array.series(bndl.size, 0, strum.abs);
				~schedBundleArray.(lag, strumOffset, server, bndl, ~latency);
			};

			if (hasGate and: { midicmd === \noteOn }) {
				noteoffs = ~midiEventFunctions[\noteOff].valueEnvir.asCollection.asControlInput.flop;
				noteoffs = noteoffs.collect({ arg args;
					vst.noteOffMsg(*args);
				});

				if (strum == 0) {
					~schedBundleArray.(lag, sustain + offset, server, noteoffs, ~latency);
				} {
					if (strum < 0) { noteoffs = noteoffs.reverse };
					if (~strumEndsTogether) {
						strumOffset = sustain + offset
					} {
						strumOffset = sustain + strumOffset
					};
					~schedBundleArray.(lag, strumOffset, server, noteoffs, ~latency);
				};

			};
		});
		// custom event type for setting VST parameters:
		Event.addEventType(\vst_set, #{ arg server;
			var bndl, array, params, scan, keys, vst = ~vst.value;
			params = ~params.value;
			params.notNil.if {
				// look up parameter names/indices in the current environment
				params.do { arg key;
					var value = currentEnvironment[key];
					value.isNil.if {
						Error("Could not find parameter '%' in Event".format(key)).throw;
					};
					array = array.add(key).add(value);
				}
			} {
				// recursively collect all integer keys in a Set (to prevent duplicates),
				// then look up parameters in in the current environment.
				scan = #{ |env, keys, scan|
					env.keysDo { arg key;
						if (key.isNumber) {
							keys.add(key);
						};
					};
					// continue in proto Event
					env.proto.notNil.if {
						scan.(env.proto, keys, scan);
					};
					keys;
				};
				keys = scan.(currentEnvironment, Set(), scan);
				keys.do { arg key;
					var value = currentEnvironment[key];
					array = array.add(key).add(value);
				}
			};
			array.notNil.if {
				bndl = array.flop.collect { arg params;
					// NB: asOSCArgArray helps to resolve unwanted Rests
					// (it calls asControlInput on all the arguments)
					vst.setMsg(*params).asOSCArgArray;
				};
				~schedBundleArray.value(~lag, ~timingOffset, server, bndl, ~latency);
			}
		});
	}
	*guiClass {
		^VSTPluginGui;
	}
	*new { arg synth, id, synthDef, wait= -1;
		var plugins, desc, info;
		// if the synthDef is nil, we try to get the metadata from the global SynthDescLib
		plugins = this.prFindPlugins(synth, synthDef);
		id.notNil.if {
			// try to find VSTPlugin with given ID
			id = id.asSymbol; // !
			desc = plugins[id];
			desc ?? {
				MethodError("SynthDef '%' doesn't contain a VSTPlugin with ID '%'!".format(synth.defName, id), this).throw;
			};
		} {
			// otherwise just get the first (and only) plugin
			(plugins.size > 1).if {
				MethodError("SynthDef '%' contains more than 1 VSTPlugin - please use the 'id' argument!".format(synth.defName), this).throw;
			};
			desc = plugins.asArray[0];
		};
		info = desc.key !? { VSTPlugin.plugins(synth.server)[desc.key] };
		^super.new.init(synth, desc.index, wait, info);
	}
	*collect { arg synth, ids, synthDef, wait= -1;
		var plugins = this.prFindPlugins(synth, synthDef);
		^this.prCollect(plugins, ids, { arg desc;
			var info = desc.key !? { VSTPlugin.plugins(synth.server)[desc.key] };
			super.new.init(synth, desc.index, wait, info);
		});
	}
	*prFindPlugins { arg synth, synthDef;
		var desc, metadata, plugins;
		synthDef.notNil.if {
			metadata = synthDef.metadata;
		} {
			desc = SynthDescLib.global.at(synth.defName.asSymbol); // for SC 3.6 compat
			desc.isNil.if { MethodError("couldn't find SynthDef '%' in global SynthDescLib!".format(synth.defName), this).throw };
			metadata = desc.metadata; // take metadata from SynthDesc, not SynthDef (SC bug)!
		};
		plugins = metadata[\vstplugins];
		(plugins.size == 0).if { MethodError("SynthDef '%' doesn't contain a VSTPlugin!".format(synth.defName), this).throw; };
		^plugins;
	}
	*prCollect { arg plugins, ids, fn;
		var result = ();
		ids.notNil.if {
			ids.do { arg key;
				var value;
				key = key.asSymbol; // !
				value = plugins.at(key);
				value.notNil.if {
					result.put(key, fn.(value));
				} { "can't find VSTPlugin with ID %".format(key).warn; }
			}
		} {
			// empty Array or nil -> get all plugins, except those without ID (shouldn't happen)
			plugins.pairsDo { arg key, value;
				(key.class == Symbol).if {
					result.put(key, fn.(value));
				} { "ignoring VSTPlugin without ID".warn; }
			}
		};
		^result;
	}

	init { arg synth, index, wait, info;
		this.synth = synth;
		this.synthIndex = index;
		this.info = info;
		this.wait = wait;
		loading = false;
		window = false;
		needQueryParams = true;
		needQueryPrograms = true;
		deferred = false;
		midi = VSTPluginMIDIProxy(this);
		oscFuncs = List.new;
		// parameter changed:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			var index, value, display;
			index = msg[3].asInteger;
			value = msg[4].asFloat;
			(msg.size > 5).if {
				display = this.class.msg2string(msg, 5);
			};
			// cache parameter value
			(index < parameterCache.size).if {
				parameterCache[index] = [value, display];
				// notify dependants
				this.changed(\param, index, value, display);
			} { "parameter index % out of range!".format(index).warn };
		}, '/vst_param'));
		// current program:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			program = msg[3].asInteger;
			// notify dependants
			this.changed(\program_index, program);
		}, '/vst_program_index'));
		// program name:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			var index, name;
			index = msg[3].asInteger;
			name = this.class.msg2string(msg, 4);
			(index < programCache.size).if {
				programCache[index] = name;
				// notify dependants
				this.changed(\program_name, index, name);
			} { "program number % out of range!".format(index).warn };
		}, '/vst_program'));
		// parameter automated:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			var index, value;
			index = msg[3].asInteger;
			value = msg[4].asFloat;
			parameterAutomated.value(index, value);
			this.changed(\automated, index, value);
		}, '/vst_auto'));
		// latency changed:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			latency = msg[3].asInteger;
			latencyChanged.value(latency);
			this.changed(\latency, latency);
		}, '/vst_latency'));
		// update display:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			this.prQueryParams;
		}, '/vst_update'));
		// plugin crashed
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			"plugin '%' crashed".format(this.info.name).warn;
			this.close;
			pluginCrashed.value;
			this.changed(\crashed);
		}, '/vst_crash'));
		// MIDI received:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			// convert to integers and pass as args to action
			midiReceived.value(*Int32Array.newFrom(msg[3..]));
		}, '/vst_midi'));
		// sysex received:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			// convert to Int8Array and pass to action
			sysexReceived.value(Int8Array.newFrom(msg[3..]));
		}, '/vst_sysex'));
		// cleanup after synth has been freed:
		synth.onFree { this.prFree };
	}
	prFree {
		// "VSTPluginController: synth freed!".postln;
		browser !? { browser.close };
		oscFuncs.do { arg func;
			func.free;
		};
		this.prClear;
		this.changed(\free);
	}
	prCheckPlugin { arg method;
		this.isOpen.not.if { MethodError("%: no plugin!".format(method.name), this).throw }
	}
	prCheckLocal { arg method;
		synth.server.isLocal.not.if {
			MethodError("'%' only works with a local Server".format(method.name), this).throw;
		}
	}
	editor { arg show=true;
		window.if { this.sendMsg('/vis', show.asInteger); }
		{ "no editor!".postln; };
	}
	editorMsg { arg show=true;
		^this.makeMsg('/vis', show.asInteger);
	}
	moveEditor { arg x, y;
		^this.sendMsg('/pos', x.asInteger, y.asInteger);
	}
	moveEditorMsg { arg x, y;
		^this.makeMsg('/pos', x.asInteger, y.asInteger);
	}
	resizeEditor { arg w, h;
		^this.sendMsg('/size', w.asInteger, h.asInteger);
	}
	resizeEditorMsg { arg w, h;
		^this.makeMsg('/size', w.asInteger, h.asInteger);
	}
	haveEditor { ^window; }
	gui { arg parent, bounds, params=true;
		^this.class.guiClass.new(this).gui(parent, bounds, params);
	}
	browse {
		// prevent opening the dialog multiple times
		browser.isNil.if {
			// create dialog
			browser = VSTPluginGui.prMakePluginBrowser(this);
			browser.view.addAction({ browser = nil }, 'onClose');
		};
		browser.front;
	}
	open { arg path, editor=true, verbose=false, action, multiThreading=false, mode;
		var intMode = 0;
		loading.if {
			// should be rather throw an Error?
			"VSTPluginController: already opening another plugin".warn;
			action.value(this, false);
			^this;
		};
		// if path is nil we try to get it from VSTPlugin
		path ?? {
			this.info !? { path = this.info.key } ?? {
				MethodError("'path' is nil but VSTPlugin doesn't have a plugin info", this).throw;
			}
		};
		// multi-threading is not supported with Supernova
		multiThreading.if {
			Server.program.find("supernova").notNil.if {
				"'multiThreading' option is not supported on Supernova; use ParGroup instead.".warn;
			}
		};
		// check mode
		mode.notNil.if {
			intMode = switch(mode.asSymbol,
				\auto, 0,
				\sandbox, 1,
				\bridge, 2,
				{ MethodError("bad value '%' for 'mode' argument".format(mode), this).throw; }
			);
		};
		// *now* we can start loading
		loading = true;
		VSTPlugin.prQuery(synth.server, path, wait, { arg info;
			info.notNil.if {
				this.prClear;
				loading = true; // HACK for prClear!
				this.prMakeOscFunc({ arg msg;
					var loaded = msg[3].asBoolean;
					loaded.if {
						window = msg[4].asBoolean;
						latency = msg[5].asInteger;
						this.info = info; // now set 'info' property
						info.addDependant(this);

						parameterCache = Array.fill(info.numParameters, [0, nil]);
						this.prQueryParams;

						// copy default program names (might change later when loading banks)
						programCache = info.programs.collect(_.name);
						needQueryPrograms = false; // !
						program = 0;

						// post info if wanted
						verbose.if { info.print };
					} {
						// shouldn't happen because /open is only sent if the plugin has been successfully probed
						"couldn't open '%'".format(path).error;
					};
					loading = false;
					deferred = multiThreading || (mode.asSymbol != \auto) || info.bridged;
					this.changed(\open, path, loaded);
					action.value(this, loaded);
					// report latency (if loaded)
					latency !? { latencyChanged.value(latency); }
				}, '/vst_open').oneShot;
				// don't set 'info' property yet; use original path!
				this.sendMsg('/open', path.asString.standardizePath,
					editor.asInteger, multiThreading.asInteger, intMode);
			} {
				"couldn't open '%'".format(path).error;
				// just notify failure, but keep old plugin (if present)
				loading = false;
				action.value(this, false);
			};
		});
	}
	openMsg { arg path, editor=false, multiThreading=false, mode;
		var intMode = 0;
		// if path is nil we try to get it from VSTPlugin
		path ?? {
			this.info !? { path = this.info.key } ?? {
				MethodError("'path' is nil but VSTPlugin doesn't have a plugin info", this).throw;
			}
		};
		// check mode
		mode.notNil.if {
			intMode = switch(mode.asSymbol,
				\auto, 0,
				\sandbox, 1,
				\bridge, 2,
				{ MethodError("bad value '%' for 'mode' argument".format(mode), this).throw; }
			);
		};
		^this.makeMsg('/open', path.asString.standardizePath,
			editor.asInteger, multiThreading.asInteger, intMode);
	}
	isOpen {
		^this.info.notNil;
	}
	// deprecated in favor of isOpen
	loaded {
		this.deprecated(thisMethod, this.class.findMethod(\isOpen));
		^this.isOpen;
	}
	prClear {
		info !? { info.removeDependant(this) };
		window = false; latency = nil; info = nil;
		parameterCache = nil; needQueryParams = false;
		programCache = nil; needQueryPrograms = true;
		program = nil; currentPreset = nil; loading = false;
	}
	addDependant { arg dependant;
		super.addDependant(dependant);
		// query after adding dependant!
		this.isOpen.if {
			needQueryParams.if { this.prQueryParams };
			needQueryPrograms.if { this.prQueryPrograms };
		};

	}
	update { arg who, what ... args;
		((who === info) and: { what == \presets }).if {
			currentPreset !? {
				info.prPresetIndex(currentPreset).isNil.if {
					currentPreset = nil;
					// "updated current preset".postln;
				}
			}
		}
	}
	close {
		this.sendMsg('/close');
		this.prClear;
		this.changed(\close);
	}
	closeMsg {
		^this.makeMsg('/close');
	}
	reset { arg async = false;
		this.sendMsg('/reset', async.asInteger);
	}
	resetMsg { arg async = false;
		^this.makeMsg('/reset', async.asInteger);
	}
	// deprecated
	setOffline { arg bool;
		this.deprecated(thisMethod);
	}
	setOfflineMsg { arg bool;
		this.deprecated(thisMethod);
		^this.makeMsg('/mode',  bool.asInteger);
	}
	// parameters
	numParameters {
		^(info !? (_.numParameters) ?? 0);
	}
	set { arg ...args;
		this.sendMsg('/set', *args);
	}
	setMsg { arg ...args;
		^this.makeMsg('/set', *args);
	}
	setn { arg ...args;
		synth.server.listSendMsg(this.setnMsg(*args));
	}
	setnMsg { arg ...args;
		var nargs = List.new;
		args.pairsDo { arg index, values;
			values.isArray.if {
				nargs.addAll([index, values.size]++ values);
			} {
				nargs.addAll([index, 1, values]);
			};
		};
		^this.makeMsg('/setn', *nargs);
	}
	get { arg index, action;
		var name;
		// We need to match the reply message against the 'index'
		// argument to avoid responding to other 'get' requests.
		// Although the UGen can handle parameter names, we must
		// first resolve it, because the reply message only
		// includes the parameter *index* (as a float).
		index.isNumber.not.if {
			name = index;
			index = info.findParamIndex(index);
			index ?? {
				MethodError("unknown parameter '%'".format(name), this).throw;
			};
		};
		this.prMakeOscFunc({ arg msg;
			// msg: address, nodeID, synthIndex, index, value
			action.value(msg[4]); // only pass value
		}, '/vst_set', index.asFloat).oneShot;
		this.sendMsg('/get', index);
	}
	getn { arg index = 0, count = -1, action;
		var name;
		// see comment in 'get'
		index.isNumber.not.if {
			name = index;
			index = info.findParamIndex(index);
			index ?? {
				MethodError("unknown parameter '%'".format(name), this).throw;
			};
		};
		this.prMakeOscFunc({ arg msg;
			// msg: address, nodeID, synthIndex, index, count, values...
			action.value(msg[5..]); // only pass values
		}, '/vst_setn', index.asFloat).oneShot;
		this.sendMsg('/getn', index, count);
	}
	map { arg ... args;
		synth.server.listSendBundle(nil, this.mapMsg(*args));
	}
	mapMsg { arg ... args;
		var krVals, arVals, result;
		krVals = List.new;
		arVals = List.new;
		result = Array.new(2);
		args.pairsDo({ arg index, bus;
			bus = bus.asBus;
			switch(bus.rate)
			{ \control } {
				krVals.addAll([index, bus.index, bus.numChannels])
			}
			{ \audio } {
				arVals.addAll([index, bus.index, bus.numChannels])
			};
			// no default case, ignore others
		});
		(krVals.size > 0).if { result = result.add(this.makeMsg('/map', *krVals)) };
		(arVals.size > 0).if { result = result.add(this.makeMsg('/mapa', *arVals)) };
		^result;
	}
	mapn { arg ... args;
		this.sendMsg('/map', *args);
	}
	mapnMsg { arg ...args;
		^this.makeMsg('/map', *args);
	}
	mapan { arg ... args;
		this.sendMsg('/mapa', *args);
	}
	mapanMsg { arg ...args;
		^this.makeMsg('/mapa', *args);
	}
	unmap { arg ...args;
		this.sendMsg('/unmap', *args);
	}
	unmapMsg { arg ...args;
		^this.makeMsg('/unmap', *args);
	}
	// preset management
	preset {
		^currentPreset;
	}
	savePreset { arg preset, action, async=true;
		var result, name, path;
		this.prCheckLocal(thisMethod);
		this.prCheckPlugin(thisMethod);

		File.mkdir(info.presetFolder); // make sure that the folder exists!
		// if 'preset' is omitted, use the last preset (if possible)
		preset.isNil.if {
			currentPreset.notNil.if {
				(currentPreset.type != \user).if {
					MethodError("current preset is not writeable!", this).throw;
				};
				name = currentPreset.name;
			} { MethodError("no current preset", this).throw };
		} {
			preset.isKindOf(Event).if {
				(preset.type != \user).if {
					MethodError("preset '%' is not writeable!".format(preset.name), this).throw;
				};
				name = preset.name;
			} {
				preset.isNumber.if {
					// 'preset' can also be an index
					result = info.presets[preset.asInteger];
					result ?? {
						MethodError("preset index % out of range!".format(preset), this).throw
					};
					(result.type != \user).if {
						MethodError("preset % is not writeable!".format(preset), this).throw
					};
					name = result.name;
				} {
					// or a (new) name
					name = preset.asString;
				}
			}
		};
		path = info.presetPath(name);

		this.writeProgram(path, { arg self, success;
			var index;
			success.if {
				index = info.addPreset(name, path);
				currentPreset = info.presets[index];
				this.changed(\preset_save, index);
			} { "couldn't save preset '%'".format(name).error };
			action.value(self, success);
		}, async);
	}
	loadPreset { arg preset, action, async=true;
		var result;
		this.prCheckLocal(thisMethod);
		this.prCheckPlugin(thisMethod);

		result = this.prGetPreset(preset);
		this.readProgram(result.path, { arg self, success;
			var index;
			success.if {
				index = info.prPresetIndex(result);
				index.notNil.if {
					currentPreset = result;
					this.changed(\preset_load, index);
				} {
					"preset '%' has been removed!".format(result.name).error;
				};
			} { "couldn't load preset".error };
			action.value(self, success);
		}, async);
	}
	loadPresetMsg { arg preset, async=true;
		var result;
		this.prCheckLocal(thisMethod);
		this.prCheckPlugin(thisMethod);

		result = this.prGetPreset(preset); // throws on error
		^this.readProgramMsg(result.path, async);
	}
	prGetPreset { arg preset;
		var result;
		// if 'preset' is omitted, use the last preset (if possible)
		preset.isNil.if {
			currentPreset.notNil.if {
				result = currentPreset;
			} { MethodError("no current preset", this).throw };
		} {
			preset.isKindOf(Event).if {
				result = preset;
			} {
				preset.isNumber.if {
					// 'preset' can also be an index
					result = info.presets[preset.asInteger];
					result ?? {
						MethodError("preset index % out of range".format(preset), this).throw;
					};
				} {
					// or a name
					result = info.findPreset(preset.asString);
					result ?? {
						MethodError("couldn't find preset '%'".format(preset), this).throw;
					};
				}
			}
		}
		^result;
	}
	deletePreset { arg preset;
		var current = false;
		var name = preset.isKindOf(Event).if { preset.name } { preset };
		this.prCheckLocal(thisMethod);
		this.prCheckPlugin(thisMethod);

		// if 'preset' is omitted, use the last preset (if possible)
		preset.isNil.if {
			currentPreset.notNil.if {
				preset = currentPreset;
				current = true;
			} { MethodError("no current preset", this).throw };
		};
		info.deletePreset(preset).if {
			current.if { currentPreset = nil };
			^true;
		}
		^false;
	}
	renamePreset { arg preset, name;
		var oldname = preset.isKindOf(Event).if { preset.name } { preset };
		this.prCheckLocal(thisMethod);
		this.prCheckPlugin(thisMethod);

		// if 'preset' is omitted, use the last preset (if possible)
		preset.isNil.if {
			currentPreset.notNil.if {
				preset = currentPreset;
			} { MethodError("no current preset", this).throw };
		};
		info.renamePreset(preset, name).if { ^true } {
			"couldn't rename preset '%'".format(oldname).error; ^false;
		}
	}
	numPrograms {
		^(info !? (_.numPrograms) ?? 0);
	}
	program_ { arg number;
		((number >= 0) && (number < this.numPrograms)).if {
			this.sendMsg('/program_set', number);
			program = number; // update!
			// notify dependends
			this.changed(\program_index, number);
			this.prQueryParams(needFork: true); // never block!
		} {
			MethodError("program number % out of range".format(number), this).throw;
		};
	}
	programMsg { arg number;
		// we can't do bound checking here
		^this.makeMsg('/program_set', number);
	}
	programName {
		this.program.notNil.if {
			^programCache[this.program];
		} { ^nil };
	}
	programName_ { arg name;
		this.sendMsg('/program_name', name);
	}
	programNameMsg { arg name;
		^this.makeMsg('/program_name', name);
	}
	readProgram { arg path, action, async=true;
		path = path.asString.standardizePath;
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
			this.prQueryParams;
		}, '/vst_program_read').oneShot;
		this.sendMsg('/program_read', path, async.asInteger);
	}
	readProgramMsg { arg dest, async=true;
		^this.makeMsg('/program_read', VSTPlugin.prMakeDest(dest), async.asInteger);
	}
	readBank { arg path, action, async=true;
		path = path.asString.standardizePath;
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
			this.prQueryParams;
			this.prQueryPrograms;
		}, '/vst_bank_read').oneShot;
		this.sendMsg('/bank_read', path, async.asInteger);
	}
	readBankMsg { arg dest, async=true;
		^this.makeMsg('/bank_read', VSTPlugin.prMakeDest(dest), async.asInteger);
	}
	writeProgram { arg path, action, async=true;
		path = path.asString.standardizePath;
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
		}, '/vst_program_write').oneShot;
		this.sendMsg('/program_write', path, async.asInteger);
	}
	writeProgramMsg { arg dest, async=true;
		^this.makeMsg('/program_write', VSTPlugin.prMakeDest(dest), async.asInteger);
	}
	writeBank { arg path, action, async=true;
		path = path.asString.standardizePath;
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
		}, '/vst_bank_write').oneShot;
		this.sendMsg('/bank_write', path, async.asInteger);
	}
	writeBankMsg { arg dest, async=true;
		^this.makeMsg('/bank_write', VSTPlugin.prMakeDest(dest), async.asInteger);
	}
	setProgramData { arg data, action, async=true;
		this.prCheckLocal(thisMethod);
		(data.class != Int8Array).if { MethodError("'%' expects Int8Array!".format(thisMethod.name), this).throw};
		this.prSetData(data, action, false, async);
	}
	setBankData { arg data, action, async=true;
		this.prCheckLocal(thisMethod);
		(data.class != Int8Array).if { MethodError("'%' expects Int8Array!".format(thisMethod.name), this).throw};
		this.prSetData(data, action, true, async);
	}
	prSetData { arg data, action, bank, async;
		try {
			var path = VSTPlugin.prMakeTmpPath;
			var cb = { arg self, success;
				// 3) call action and delete file
				action.value(self, success);
				File.delete(path).not.if { ("Could not delete data file:" + path).warn };
			};
			// 1) write data to file
			File.use(path, "wb", { arg file; file.write(data) });
			// 2) ask plugin to read data file
			bank.if { this.readBank(path, cb, async) } { this.readProgram(path, cb, async) };
		} { MethodError("Failed to write data", this).throw };
	}
	sendProgramData { arg data, wait, action, async=true;
		wait = wait ?? this.wait;
		(data.class != Int8Array).if { MethodError("'%' expects Int8Array!".format(thisMethod.name), this).throw};
		this.prSendData(data, wait, action, false, async);
	}
	sendBankData { arg data, wait, action, async=true;
		wait = wait ?? this.wait;
		(data.class != Int8Array).if { MethodError("'%' expects Int8Array!".format(thisMethod.name), this).throw};
		this.prSendData(data, wait, action, true, async);
	}
	prSendData { arg data, wait, action, bank, async;
		// stream preset data to the plugin via a Buffer.
		// wait = -1 allows an OSC roundtrip between packets.
		// wait = 0 might not be safe in a high traffic situation,
		// maybe okay with tcp.
		var buffer, sym;
		this.prCheckPlugin(thisMethod);
		wait = wait ?? this.wait;
		sym = bank.if {'bank' } {'program'};

		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			buffer.free;
			action.value(this, success);
			this.prQueryParams(wait);
			bank.if { this.prQueryPrograms(wait) };
		}, "/vst_"++sym++"_read").oneShot;

		buffer = Buffer.sendCollection(synth.server, data, wait: wait, action: { arg buf;
			this.sendMsg("/"++sym++"_read", VSTPlugin.prMakeDest(buf), async.asInteger);
		});
	}
	getProgramData { arg action, async=true;
		this.prCheckLocal(thisMethod);
		this.prGetData(action, false, async);
	}
	getBankData { arg action, async=true;
		this.prCheckLocal(thisMethod);
		this.prGetData(action, true, async);
	}
	prGetData { arg action, bank, async;
		var path = VSTPlugin.prMakeTmpPath;
		var cb = { arg self, success;
			// 2) when done, try to read data file, pass data to action and delete file
			var data;
			success.if {
				try {
					File.use(path, "rb", { arg file;
						data = Int8Array.newClear(file.length);
						file.read(data);
					});
				} { "Failed to read data".error };
				File.delete(path).not.if { ("Could not delete data file:" + path).warn };
			} { "Couldn't read data file".warn };
			// done (on fail, data is nil)
			action.value(data);
		};
		// 1) ask plugin to write data file
		bank.if { this.writeBank(path, cb, async) } { this.writeProgram(path, cb, async.asInteger) };
	}
	receiveProgramData { arg wait, timeout=3, action, async=true;
		this.prReceiveData(wait, timeout, action, false);
	}
	receiveBankData { arg wait, timeout=3, action, async=true;
		this.prReceiveData(wait, timeout, action, true);
	}
	prReceiveData { arg wait, timeout, action, bank, async;
		// stream data from the plugin via a Buffer.
		// wait = -1 allows an OSC roundtrip between packets.
		// wait = 0 might not be safe in a high traffic situation,
		// maybe okay with tcp.
		var address, sym;
		this.prCheckPlugin(thisMethod);
		wait = wait ?? this.wait;
		sym = bank.if {'bank' } {'program'};
		{
			var buf = Buffer(synth.server); // get free Buffer
			// ask VSTPlugin to store the preset data in this Buffer
			// (it will allocate the memory for us!)
			this.sendMsg("/"++sym++"_write", VSTPlugin.prMakeDest(buf), async);
			// wait for cmd to finish and update buffer info
			synth.server.sync;
			buf.updateInfo({
				// now read data from Buffer
				buf.getToFloatArray(wait: wait, timeout: timeout, action: { arg array;
					var data;
					(array.size > 0).if {
						data = array.as(Int8Array);
					};
					buf.free;
					action.value(data); // done
				});
			});
		}.forkIfNeeded;
	}
	// MIDI / Sysex
	sendMidi { arg status, data1=0, data2=0, detune;
		// LATER we might actually omit detune if nil
		this.sendMsg('/midi_msg', Int8Array.with(status, data1, data2), detune ?? 0.0);
	}
	sendMidiMsg { arg status, data1=0, data2=0, detune;
		^this.makeMsg('/midi_msg', Int8Array.with(status, data1, data2), detune ?? 0.0);
	}
	sendSysex { arg msg;
		synth.server.listSendMsg(this.sendSysexMsg(msg));
	}
	sendSysexMsg { arg msg;
		(msg.class != Int8Array).if { MethodError("'%' expects Int8Array!".format(thisMethod.name), this).throw };
		(msg.size > oscPacketSize).if {
			"sending sysex data larger than % bytes is risky".format(oscPacketSize).warn;
		};
		^this.makeMsg('/midi_sysex', msg);
	}
	// transport
	setTempo { arg bpm=120;
		this.sendMsg('/tempo', bpm);
	}
	setTempoMsg { arg bpm=120;
		^this.makeMsg('/tempo', bpm);
	}
	setTimeSignature { arg num=4, denom=4;
		this.sendMsg('/time_sig', num, denom);
	}
	setTimeSignatureMsg { arg num=4, denom=4;
		^this.makeMsg('/time_sig', num, denom);
	}
	setPlaying { arg b;
		this.sendMsg('/transport_play', b.asInteger);
	}
	setPlayingMsg { arg b;
		^this.makeMsg('/transport_play', b.asInteger);
	}
	setTransportPos { arg pos;
		this.sendMsg('/transport_set', pos);
	}
	setTransportPosMsg { arg pos;
		^this.makeMsg('/transport_set', pos);
	}
	getTransportPos { arg action;
		this.prMakeOscFunc({ arg msg;
			action.value(msg[3]);
		}, '/vst_transport').oneShot;
		this.sendMsg('/transport_get');
	}
	// advanced
	canDo { arg what, action;
		this.prMakeOscFunc({ arg msg;
			action.value(msg[3].asInteger);
		}, '/vst_can_do').oneShot;
		this.sendMsg('/can_do', what);
	}
	vendorMethod { arg index=0, value=0, ptr, opt=0.0, action, async=true;
		this.prMakeOscFunc({ arg msg;
			action.value(msg[3].asInteger);
		}, '/vst_vendor_method').oneShot;
		this.sendMsg('/vendor_method', index.asInteger, value.asInteger,
			ptr.as(Int8Array), opt.asFloat, async.asInteger);
	}
	vendorMethodMsg { arg index=0, value=0, ptr, opt=0.0, action, async=true;
		^this.makeMsg('/vendor_method', index.asInteger, value.asInteger,
			ptr.as(Int8Array), opt.asFloat, async.asInteger);
	}
	// internal
	sendMsg { arg cmd ... args;
		synth.server.sendMsg('/u_cmd', synth.nodeID, synthIndex, cmd, *args);
	}
	makeMsg { arg cmd ... args;
		^['/u_cmd', synth.nodeID, synthIndex, cmd] ++ args;
	}
	prMakeOscFunc { arg func, path ... argTemplate;
		^OSCFunc(func, path, synth.server.addr, argTemplate: [synth.nodeID, synthIndex] ++ argTemplate);
	}
	*msg2string { arg msg, onset=0;
		// format: len, chars...
		var len = msg[onset].asInteger;
		(len > 0).if {
			^msg[(onset+1)..(onset+len)].collectAs({arg item; item.asInteger.asAscii}, String);
		} { ^"" };
	}
	prQueryParams { arg wait, needFork=false;
		(this.dependants.size > 0).if {
			needFork.if {
				fork {
					// make sure that values/displays are really up-to-date!
					deferred.if { synth.server.sync };
					this.prQuery(wait, this.numParameters, '/param_query');
				}
			} {
				forkIfNeeded {
					this.prQuery(wait, this.numParameters, '/param_query');
				}
			};
			needQueryParams = false;
		} { needQueryParams = true; }
	}
	prQueryPrograms { arg wait;
		(this.dependants.size > 0).if {
			forkIfNeeded {
				this.prQuery(wait, this.numPrograms, '/program_query');
			};
			needQueryPrograms = false;
		} { needQueryPrograms = true; }
	}
	prQuery { arg wait, num, cmd;
		var div, mod;
		div = num.div(16);
		mod = num.mod(16);
		wait = wait ?? this.wait;
		// request 16 parameters/programs at once
		div.do { arg i;
			this.sendMsg(cmd, i * 16, 16);
			if (wait >= 0) { wait.wait } { synth.server.sync };
		};
		// request remaining parameters/programs
		(mod > 0).if { this.sendMsg(cmd, num - mod, mod) };
	}
}

// mimicks MIDIOut
VSTPluginMIDIProxy {
	var <>owner;
	// dummy variable for backwards compatibility with \midi Event type (use \vst_midi instead!)
	var <>uid;

	*new { arg theOwner;
		^super.new.owner_(theOwner);
	}
	// dummy setter/getter for compatibility with MIDIOut
	latency { ^0; }
	latency_ {
		"Calling 'latency' on VSTPluginMIDIProxy has no effect.\n"
		"If you want to schedule MIDI messages on the Server, use Server.bind or similar methods.".warn;
		^this;
	}
	write { arg len, hiStatus, loStatus, a=0, b=0, detune;
		owner.sendMidi(hiStatus bitOr: loStatus, a, b, detune);
	}
	writeMsg { arg len, hiStatus, loStatus, a=0, b=0, detune;
		^owner.sendMidiMsg(hiStatus bitOr: loStatus, a, b, detune);
	}
	noteOn { arg chan, note=60, veloc=64;
		this.write(3, 16r90, chan.asInteger, note.asInteger, veloc.asInteger, note.frac * 100);
	}
	noteOnMsg { arg chan, note=60, veloc=64;
		^this.writeMsg(3, 16r90, chan.asInteger, note.asInteger, veloc.asInteger, note.frac * 100);
	}
	noteOff { arg chan, note=60, veloc=64;
		this.write(3, 16r80, chan.asInteger, note.asInteger, veloc.asInteger, note.frac * 100);
	}
	noteOffMsg { arg chan, note=60, veloc=64;
		^this.writeMsg(3, 16r80, chan.asInteger, note.asInteger, veloc.asInteger, note.frac * 100);
	}
	polyTouch { arg chan, note=60, val=64;
		this.write(3, 16rA0, chan.asInteger, note.asInteger, val.asInteger);
	}
	polyTouchMsg { arg chan, note=60, val=64;
		^this.writeMsg(3, 16rA0, chan.asInteger, note.asInteger, val.asInteger);
	}
	control { arg chan, ctlNum=7, val=64;
		this.write(3, 16rB0, chan.asInteger, ctlNum.asInteger, val.asInteger);
	}
	controlMsg { arg chan, ctlNum=7, val=64;
		^this.writeMsg(3, 16rB0, chan.asInteger, ctlNum.asInteger, val.asInteger);
	}
	program { arg chan, num=1;
		this.write(2, 16rC0, chan.asInteger, num.asInteger);
	}
	programMsg { arg chan, num=1;
		^this.writeMsg(2, 16rC0, chan.asInteger, num.asInteger);
	}
	touch { arg chan, val=64;
		this.write(2, 16rD0, chan.asInteger, val.asInteger);
	}
	touchMsg { arg chan, val=64;
		^this.writeMsg(2, 16rD0, chan.asInteger, val.asInteger);
	}
	bend { arg chan, val=8192;
		val = val.asInteger;
		this.write(3, 16rE0, chan, val bitAnd: 127, val >> 7);
	}
	bendMsg { arg chan, val=8192;
		val = val.asInteger;
		^this.writeMsg(3, 16rE0, chan, val bitAnd: 127, val >> 7);
	}
	allNotesOff { arg chan;
		this.control(chan, 123, 0);
	}
	allNotesOffMsg { arg chan;
		^this.controlMsg(chan, 123, 0);
	}
	smpte	{ arg frames=0, seconds=0, minutes=0, hours=0, frameRate = 3;
		var packet;
		packet = [frames, seconds, minutes, hours]
			.asInteger
			.collect({ arg v, i; [(i * 2 << 4) | (v & 16rF), (i * 2 + 1 << 4) | (v >> 4) ] });
		packet = packet.flat;
		packet.put(7, packet.at(7) | ( frameRate << 1 ) );
		packet.do({ arg v; this.write(2, 16rF0, 16r01, v); });
	}
	// smpteMsg not practicable
	songPtr { arg songPtr;
		songPtr = songPtr.asInteger;
		this.write(4, 16rF0, 16r02, songPtr & 16r7f, songPtr >> 7 & 16r7f);
	}
	songPtrMsg { arg songPtr;
		songPtr = songPtr.asInteger;
		^this.writeMsg(4, 16rF0, 16r02, songPtr & 16r7f, songPtr >> 7 & 16r7f);
	}
	songSelect { arg song;
		this.write(3, 16rF0, 16r03, song.asInteger);
	}
	songSelectMsg { arg song;
		^this.writeMsg(3, 16rF0, 16r03, song.asInteger);
	}
	midiClock {
		this.write(1, 16rF0, 16r08);
	}
	midiClockMsg {
		^this.writeMsg(1, 16rF0, 16r08);
	}
	start {
		this.write(1, 16rF0, 16r0A);
	}
	startMsg {
		^this.writeMsg(1, 16rF0, 16r0A);
	}
	continue {
		this.write(1, 16rF0, 16r0B);
	}
	continueMsg {
		^this.writeMsg(1, 16rF0, 16r0B);
	}
	stop {
		this.write(1, 16rF0, 16r0C);
	}
	stopMsg {
		^this.writeMsg(1, 16rF0, 16r0C);
	}
	reset {
		this.write(1, 16rF0, 16r0F);
	}
	resetMsg {
		^this.writeMsg(1, 16rF0, 16r0F);
	}
	sysex { arg packet;
		owner.sendSysex(packet);
	}
	sysexMsg { arg packet;
		^owner.sendSysexMsg(packet);
	}
}
