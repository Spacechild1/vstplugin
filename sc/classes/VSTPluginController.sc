VSTPluginController {
	// class var
	const oscPacketSize = 1600; // safe max. OSC packet size
	// public
	var <synth;
	var <synthIndex;
	var <loaded;
	var <info;
	var <midi;
	var <program;
	var <>wait;
	// callbacks
	var <>parameterAutomated;
	var <>midiReceived;
	var <>sysexReceived;
	// private
	var oscFuncs;
	var <paramCache; // only for dependants
	var <programNames; // only for dependants
	var currentPreset; // the current preset
	var window; // do we have a VST editor?
	var loading; // are we currently loading a plugin?
	var browser; // handle to currently opened browser
	var didQuery; // do we need to query parameters?

	*initClass {
		Class.initClassTree(Event);
		// custom event type for playing VSTis:
		Event.addEventType(\vst_midi, #{ arg server;
			var freqs, lag, offset, strumOffset, dur, sustain, strum;
			var bndl, noteoffs, vst, hasGate, midicmd;

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
			bndl = ~midiEventFunctions[midicmd].valueEnvir.asCollection.flop;
			bndl = bndl.collect({ arg args;
				vst.performList((midicmd ++ "Msg").asSymbol, args);
			});

			if (strum == 0) {
				~schedBundleArray.(lag, offset, server, bndl, ~latency);
			} {
				if (strum < 0) { bndl = bndl.reverse };
				strumOffset = offset + Array.series(bndl.size, 0, strum.abs);
				~schedBundleArray.(lag, strumOffset, server, bndl, ~latency);
			};

			if (hasGate and: { midicmd === \noteOn }) {
				noteoffs = ~midiEventFunctions[\noteOff].valueEnvir.asCollection.flop;
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
			var bndl, msgFunc, getParams, params, vst;
			// custom version of envirPairs/envirGet which also supports integer keys (for parameter indices)
			getParams = #{ arg array;
				var result = [];
				array.do { arg name;
					var value = currentEnvironment.at(name);
					value !? { result = result.add(name).add(value); };
				};
				result;
			};
			// ~params is an Array of parameter names and/or indices which are looked up in the current environment (= the Event).
			// if ~params is omitted, we try to look up *every* parameter by name (not very efficient for large plugins!)
			vst = ~vst.value;
			params = ~params.value;
			if (params.isNil) {
				params = vst.info.parameters.collect { arg p; p.name.asSymbol };
			};
			bndl = getParams.(params).flop.collect { arg params;
				vst.value.setMsg(*params);
			};
			~schedBundleArray.value(~lag, ~timingOffset, server, bndl, ~latency);
		});
	}
	*guiClass {
		^VSTPluginGui;
	}
	*new { arg synth, id, synthDef, wait= -1;
		var synthIndex, desc;
		// if the synthDef is nil, we try to get it from the global SynthDescLib
		synthDef.isNil.if {
			desc = SynthDescLib.global.at(synth.defName);
			desc.isNil.if { ^"couldn't find synthDef in global SynthDescLib!".throw };
			synthDef = desc.def;
		};
		// walk the list of UGens and get the VSTPlugin instance which matches the given ID.
		// if 'id' is nil, pick the first instance. throw an error if no VSTPlugin is found.
		synthDef.children.do { arg ugen, index;
			(ugen.class == VSTPlugin).if {
				(id.isNil || (ugen.id == id)).if {
					^super.new.init(synth, index, wait, ugen.info);
				}
			};
		};
		id.isNil.if {^"synth doesn't contain a VSTPlugin!".throw;}
		{^"synth doesn't contain a VSTPlugin with ID '%'".format(id).throw;}
	}
	init { arg theSynth, theIndex, waitTime, theInfo;
		synth = theSynth;
		synthIndex = theIndex;
		wait = waitTime;
		info = theInfo;
		loaded = false;
		loading = false;
		window = false;
		didQuery = false;
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
			(index < paramCache.size).if {
				paramCache[index] = [value, display];
				// notify dependants
				this.changed('/param', index, value, display);
			} { ^"parameter index % out of range!".format(index).warn };
		}, '/vst_param'));
		// current program:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			program = msg[3].asInteger;
			// notify dependants
			this.changed('/program_index', program);
		}, '/vst_program_index'));
		// program name:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			var index, name;
			index = msg[3].asInteger;
			name = this.class.msg2string(msg, 4);
			(index < programNames.size).if {
				programNames[index] = name;
				// notify dependants
				this.changed('/program_name', index, name);
			} { "program number % out of range!".format(index).warn };
		}, '/vst_program'));
		// parameter automated:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			var index, value;
			index = msg[3].asInteger;
			value = msg[4].asFloat;
			parameterAutomated.value(index, value);
		}, '/vst_auto'));
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
		this.changed('/free');
	}
	editor { arg show=true;
		window.if { this.sendMsg('/vis', show.asInteger); }
		{ "no editor!".postln; };
	}
	editorMsg { arg show=true;
		^this.makeMsg('/vis', show.asInteger);
	}
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
	open { arg path, editor=false, verbose=false, action, threaded=false;
		loading.if {
			"already opening!".error;
			^this;
		};
		loading = true;
		// threaded is not supported for Supernova
		threaded.if {
			Server.program.find("supernova").notNil.if {
				"multiprocessing option not supported for Supernova; use ParGroup instead.".warn;
			}
		};
		// if path is nil we try to get it from VSTPlugin
		path ?? {
			this.info !? { path = this.info.key } ?? { ^"'path' is nil but VSTPlugin doesn't have a plugin info".throw }
		};
		path.isString.if { path = path.standardizePath };
		VSTPlugin.prGetInfo(synth.server, path, wait, { arg theInfo;
			theInfo.notNil.if {
				this.prClear;
				this.prMakeOscFunc({arg msg;
					loaded = msg[3].asBoolean;
					loaded.if {
						theInfo.notNil.if {
							window = msg[4].asBoolean;
							info = theInfo; // now set 'info' property
							info.addDependant(this);
							paramCache = Array.fill(theInfo.numParameters, [0, nil]);
							program = 0;
							// copy default program names (might change later when loading banks)
							programNames = theInfo.programs.collect(_.name);
							// only query parameters if we have dependants!
							(this.dependants.size > 0).if {
								this.prQueryParams;
							};
							// post info if wanted
							verbose.if { theInfo.print };
						} {
							"bug: got no info!".error; // shouldn't happen...
							loaded = false; window = false;
						};
					} {
						// shouldn't happen because /open is only sent if the plugin has been successfully probed
						"couldn't open '%'".format(path).error;
					};
					loading = false;
					this.changed('/open', path, loaded);
					action.value(this, loaded);
				}, '/vst_open').oneShot;
				// don't set 'info' property yet
				this.sendMsg('/open', theInfo.key, editor.asInteger, threaded.asInteger);
			} {
				"couldn't open '%'".format(path).error;
				// just notify failure, but keep old plugin (if present)
				loading = false;
				action.value(this, false);
			};
		});
	}
	openMsg { arg path, editor=false;
		// if path is nil we try to get it from VSTPlugin
		path ?? {
			this.info !? { path = this.info.key } ?? { ^"'path' is nil but VSTPlugin doesn't have a plugin info".throw }
		};
		^this.makeMsg('/open', path.asString.standardizePath, editor.asInteger);
	}
	prClear {
		info !? { info.removeDependant(this) };
		loaded = false; window = false; info = nil;	paramCache = nil; programNames = nil; didQuery = false;
		program = nil; currentPreset = nil; loading = false;
	}
	addDependant { arg dependant;
		// only query parameters for the first dependant!
		(loaded && didQuery.not).if {
			this.prQueryParams;
		};
		super.addDependant(dependant);
	}
	update { arg who, what ... args;
		((who === info) and: { what == '/presets' }).if {
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
		this.changed('/close');
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
		this.prMakeOscFunc({ arg msg;
			// msg: address, nodeID, index, value
			action.value(msg[3]); // only pass value
		}, '/vst_set').oneShot;
		this.sendMsg('/get', index);
	}
	getn { arg index = 0, count = -1, action;
		this.prMakeOscFunc({ arg msg;
			// msg: address, nodeID, index, count, values...
			action.value(msg[4..]); // only pass values
		}, '/vst_setn').oneShot;
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
	savePreset { arg preset, action, async=false;
		var result, name, path;
		synth.server.isLocal.not.if {
			^"'%' only works with a local Server".format(thisMethod.name).throw;
		};

		info.notNil.if {
			File.mkdir(info.presetFolder); // make sure that the folder exists!
			// if 'preset' is omitted, use the last preset (if possible)
			preset.isNil.if {
				currentPreset.notNil.if {
					(currentPreset.type != \user).if {
						"current preset is not writeable!".error; ^this;
					};
					name = currentPreset.name;
				} { "no current preset".error; ^this };
			} {
				// 'preset' can be an index
				preset.isNumber.if {
					preset = preset.asInteger;
					result = info.presets[preset];
					result ?? {
						"preset index % out of range".format(preset).error; ^this;
					};
					(result.type != \user).if {
						"preset % is not writeable!".format(preset).error; ^this;
					};
					name = result.name;
				} {
					// or a string
					name = preset.asString;
				}
			};
			path = info.presetPath(name);
			this.writeProgram(path, { arg self, success;
				var index;
				success.if {
					index = info.addPreset(name, path);
					currentPreset = info.presets[index];
					this.changed('/preset_save', index);
				} { "couldn't save preset".error };
				action.value(self, success);
			}, async);
		} { "no plugin loaded".error }
	}
	loadPreset { arg preset, action, async=false;
		var index, result;
		synth.server.isLocal.not.if {
			^"'%' only works with a local Server".format(thisMethod.name).throw;
		};

		info.notNil.if {
			// if 'preset' is omitted, use the last preset (if possible)
			preset.isNil.if {
				currentPreset.notNil.if {
					index = info.prPresetIndex(currentPreset);
					index ?? { ^"bug: couldn't find current preset".throw };
				} { "no current preset".error; ^this };
			} {
				preset.isNumber.if {
					index = preset.asInteger;
				} {
					// try to find by name
					index = info.prPresetIndex(preset.asString);
					index ?? { "couldn't find preset '%'".format(preset).error; ^this }
				};
			};
			result = info.presets[index];
			result.notNil.if {
				this.readProgram(result.path, { arg self, success;
					success.if {
						currentPreset = result;
						this.changed('/preset_load', index);
					} { "couldn't load preset".error };
					action.value(self, success);
				}, async);
			} { "preset index % out of range".format(preset).error };
		} { "no plugin loaded".error }
	}
	deletePreset { arg preset;
		var current = false;
		synth.server.isLocal.not.if {
			^"'%' only works with a local Server".format(thisMethod.name).throw;
		};

		info.notNil.if {
			// if 'preset' is omitted, use the last preset (if possible)
			preset.isNil.if {
				currentPreset.notNil.if {
					(currentPreset.type != \user).if {
						"current preset is not writeable!".error; ^false;
					};
					preset = currentPreset.name;
					current = true;
				} { "no current preset".error; ^false };
			};
			info.deletePreset(preset).if {
				current.if { currentPreset = nil };
				^true;
			} {
				"couldn't delete preset '%'".format(preset).error;
			}
		} { "no plugin loaded".error };
		^false;
	}
	renamePreset { arg preset, name;
		synth.server.isLocal.not.if {
			^"'%' only works with a local Server".format(thisMethod.name).throw;
		};

		info.notNil.if {
			// if 'preset' is omitted, use the last preset (if possible)
			preset.isNil.if {
				currentPreset.notNil.if {
					(currentPreset.type != \user).if {
						"current preset is not writeable!".error; ^false;
					};
					preset = currentPreset.name;
				} { "no current preset".error; ^false };
			};
			info.renamePreset(preset, name).if { ^true } {
				"couldn't rename preset '%'".format(preset).error;
			}
		} { "no plugin loaded".error };
		^false;
	}
	numPrograms {
		^(info !? (_.numPrograms) ?? 0);
	}
	program_ { arg number;
		((number >= 0) && (number < this.numPrograms)).if {
			this.sendMsg('/program_set', number);
			this.prQueryParams;
		} {
			"program number % out of range".format(number).error;
		};
	}
	programMsg { arg number;
		// we can't do bound checking here
		^this.makeMsg('/program_set', number);
	}
	programName {
		this.program.notNil.if {
			^programNames[this.program];
		} { ^nil };
	}
	programName_ { arg name;
		this.sendMsg('/program_name', name);
	}
	programNameMsg { arg name;
		^this.makeMsg('/program_name', name);
	}
	readProgram { arg path, action, async=false;
		path = path.asString.standardizePath;
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
			this.prQueryParams;
		}, '/vst_program_read').oneShot;
		this.sendMsg('/program_read', path, async.asInteger);
	}
	readProgramMsg { arg dest, async=false;
		^this.makeMsg('/program_read', VSTPlugin.prMakeDest(dest), async.asInteger);
	}
	readBank { arg path, action, async=false;
		path = path.asString.standardizePath;
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
			this.prQueryParams;
			this.prQueryPrograms;
		}, '/vst_bank_read').oneShot;
		this.sendMsg('/bank_read', path, async.asInteger);
	}
	readBankMsg { arg dest, async=false;
		^this.makeMsg('/bank_read', VSTPlugin.prMakeDest(dest), async.asInteger);
	}
	writeProgram { arg path, action, async=false;
		path = path.asString.standardizePath;
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
		}, '/vst_program_write').oneShot;
		this.sendMsg('/program_write', path, async.asInteger);
	}
	writeProgramMsg { arg dest, async=false;
		^this.makeMsg('/program_write', VSTPlugin.prMakeDest(dest), async.asInteger);
	}
	writeBank { arg path, action, async=false;
		path = path.asString.standardizePath;
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
		}, '/vst_bank_write').oneShot;
		this.sendMsg('/bank_write', path, async.asInteger);
	}
	writeBankMsg { arg dest, async=false;
		^this.makeMsg('/bank_write', VSTPlugin.prMakeDest(dest), async.asInteger);
	}
	setProgramData { arg data, action, async=false;
		(data.class != Int8Array).if {^"'%' expects Int8Array!".format(thisMethod.name).throw};
		synth.server.isLocal.if {
			this.prSetData(data, action, false, async);
		} { ^"'%' only works with a local Server".format(thisMethod.name).throw };
	}
	setBankData { arg data, action, async=false;
		(data.class != Int8Array).if {^"'%' expects Int8Array!".format(thisMethod.name).throw};
		synth.server.isLocal.if {
			this.prSetData(data, action, true, async);
		} { ^"'%' only works with a local Server".format(thisMethod.name).throw; };
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
		} { "Failed to write data".warn };
	}
	sendProgramData { arg data, wait, action, async=false;
		wait = wait ?? this.wait;
		(data.class != Int8Array).if {^"'%' expects Int8Array!".format(thisMethod.name).throw};
		this.prSendData(data, wait, action, false, async);
	}
	sendBankData { arg data, wait, action, async=false;
		wait = wait ?? this.wait;
		(data.class != Int8Array).if {^"'%' expects Int8Array!".format(thisMethod.name).throw};
		this.prSendData(data, wait, action, true, async);
	}
	prSendData { arg data, wait, action, bank, async;
		// stream preset data to the plugin via a Buffer.
		// wait = -1 allows an OSC roundtrip between packets.
		// wait = 0 might not be safe in a high traffic situation,
		// maybe okay with tcp.
		var buffer, sym;
		wait = wait ?? this.wait;
		loaded.not.if {"can't send data - no plugin loaded!".warn; ^nil };
		sym = bank.if {'bank' } {'program'};

		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			buffer.free;
			action.value(this, success);
			this.prQueryParams(wait);
			bank.if { this.prQueryPrograms(wait) };
		}, "/vst_"++sym++"_read").oneShot;

		buffer = Buffer.sendCollection(synth.server, data, wait: wait, action: { arg buf;
			this.sendMsg("/"++sym++"_read", VSTPlugin.prMakeDest(buf), async);
		});
	}
	getProgramData { arg action, async=false;
		this.prGetData(action, false, async);
	}
	getBankData { arg action, async=false;
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
			} { "Could not get data".warn };
			// done (on fail, data is nil)
			action.value(data);
		};
		// 1) ask plugin to write data file
		bank.if { this.writeBank(path, cb, async) } { this.writeProgram(path, cb, async) };
	}
	receiveProgramData { arg wait, timeout=3, action, async=false;
		this.prReceiveData(wait, timeout, action, false);
	}
	receiveBankData { arg wait, timeout=3, action, async=false;
		this.prReceiveData(wait, timeout, action, true);
	}
	prReceiveData { arg wait, timeout, action, bank, async;
		// stream data from the plugin via a Buffer.
		// wait = -1 allows an OSC roundtrip between packets.
		// wait = 0 might not be safe in a high traffic situation,
		// maybe okay with tcp.
		var address, sym;
		wait = wait ?? this.wait;
		loaded.not.if {"can't receive data - no plugin loaded!".warn; ^nil };
		sym = bank.if {'bank' } {'program'};
		{
			var buf = Buffer(synth.server); // get free Buffer
			// ask VSTPlugin to store the preset data in this Buffer
			// (it will allocate the memory for us!)
			this.sendMsg("/"++sym++"_write", VSTPlugin.prMakeDest(buf), async);
			// wait for cmd to finish and update buffer info
			synth.server.sync;
			buf.updateInfo({
				buf.postln;
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
		(msg.class != Int8Array).if {^"'%' expects Int8Array!".format(thisMethod.name).throw};
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
	vendorMethod { arg index=0, value=0, ptr, opt=0.0, action, async=false;
		this.prMakeOscFunc({ arg msg;
			action.value(msg[3].asInteger);
		}, '/vst_vendor_method').oneShot;
		this.sendMsg('/vendor_method', index.asInteger, value.asInteger,
			ptr.as(Int8Array), opt.asFloat, async.asInteger);
	}
	vendorMethodMsg { arg index=0, value=0, ptr, opt=0.0, action, async=false;
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
	prMakeOscFunc { arg func, path;
		^OSCFunc(func, path, synth.server.addr, argTemplate: [synth.nodeID, synthIndex]);
	}
	*msg2string { arg msg, onset=0;
		// format: len, chars...
		var len = msg[onset].asInteger;
		(len > 0).if {
			^msg[(onset+1)..(onset+len)].collectAs({arg item; item.asInteger.asAscii}, String);
		} { ^"" };
	}
	prQueryParams { arg wait;
		this.prQuery(wait, this.numParameters, '/param_query');
		didQuery = true;
	}
	prQueryPrograms { arg wait;
		this.prQuery(wait, this.numPrograms, '/program_query');
	}
	prQuery { arg wait, num, cmd;
		{
			var div, mod;
			div = num.div(16);
			mod = num.mod(16);
			wait = wait ?? this.wait;
			// request 16 parameters/programs at once
			div.do { arg i;
				this.sendMsg(cmd, i * 16, 16);
				if(wait >= 0) { wait.wait } { synth.server.sync };
			};
			// request remaining parameters/programs
			(mod > 0).if { this.sendMsg(cmd, num - mod, mod) };
		}.forkIfNeeded;
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
