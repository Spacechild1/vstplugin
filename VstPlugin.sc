VstPlugin : MultiOutUGen {
	// class members
	classvar <defaultSearchPaths;
	classvar <>useDefaultSearchPaths = true;
	classvar <userSearchPaths;
	classvar <platformExtensions;
	classvar <searchPathDict;
	classvar pluginDict;
	classvar parentInfo;
	// instance members
	var <id;
	// class methods
	*initClass {
		StartUp.add {
			pluginDict = IdentityDictionary.new;
			parentInfo = (
				print: { arg self, long = false;
					"---".postln;
					"name: '%'".format(self.name).postln;
					"version: %".format(self.version).postln;
					"editor: %".format(self.hasEditor).postln;
					"input channels: %".format(self.numInputs).postln;
					"output channels: %".format(self.numOutputs).postln;
					"single precision: %".format(self.singlePrecision).postln;
					"double precision: %".format(self.doublePrecision).postln;
					"MIDI input: %".format(self.midiInput).postln;
					"MIDI output: %".format(self.midiOutput).postln;
					"synth: %".format(self.isSynth).postln;
					long.if {
						"".postln;
						"parameters (%):".format(self.numParameters).postln;
						self.printParameters;
						"".postln;
						"programs (%):".format(self.numPrograms).postln;
						self.printPrograms;
					} {
						"parameters: %".format(self.numParameters);
						"programs: %".format(self.numPrograms);
					};
					"".postln;
				},
				printParameters: { arg self;
					self.numParameters.do { arg i;
						var label;
						label = (self.parameterLabels[i].size > 0).if { "(%)".format(self.parameterLabels[i]) };
						"[%] % %".format(i, self.parameterNames[i], label ?? "").postln;
					};
				},
				printPrograms: { arg self;
					self.programs.do { arg item, i;
						"[%] %".format(i, item).postln;
					};
				}
			);
		}
	}
	*ar { arg input, numOut=1, bypass=0, params, id;
		var numIn = 0;
		input.notNil.if {
			numIn = input.isArray.if {input.size} { 1 };
		}
		^this.multiNewList([\audio, id, numOut, bypass, numIn] ++ input ++ params);
	}
	*kr { ^this.shouldNotImplement(thisMethod) }

	*info { arg server;
		server = server ?? Server.default;
		^pluginDict[server];
	}
	*plugins { arg server;
		var dict = this.info(server);
		dict.notNil.if {
			// return sorted list of plugin keys (case insensitive)
			^Array.newFrom(dict.keys).sort({ arg a, b; a.asString.compare(b.asString, true) < 0});
		} { ^[] };
	}
	*print { arg server;
		this.plugins(server).do { arg p; p.postln; }
	}
	*reset { arg server;
		server = server ?? Server.default;
		// clear plugin dictionary
		pluginDict[server] = IdentityDictionary.new;
	}
	*search { arg server, path, useDefault=true, verbose=false, wait = -1, action;
		server = server ?? Server.default;
		path.isString.if { path = [path] };
		(path.isNil or: path.isArray).not.if { ^"bad type for 'path' argument!".throw };
		// add dictionary if it doesn't exist yet
		pluginDict[server].isNil.if { pluginDict[server] = IdentityDictionary.new };
		server.isLocal.if { this.prSearchLocal(server, path, useDefault, verbose, action) }
		{ this.prSearchRemote(server, path, useDefault, verbose, wait, action) };
	}
	*prSearchLocal { arg server, searchPaths, useDefault, verbose, wait, action;
		{
			var dict = pluginDict[server];
			var filePath = PathName.tmp ++ this.hash.asString;
			protect {
				// first write search paths to tmp file
				File.use(filePath, "wb", { arg file;
					searchPaths.do { arg path; file.write(path); file.write("\n"); }
				});
			} { arg error;
				error.isNil.if {
					// ask server to read tmp file and replace the content with the plugin info
					server.sendMsg('/cmd', '/vst_search', useDefault, verbose, filePath);
					// wait for cmd to finish
					server.sync;
					// read file
					protect {
						File.use(filePath, "rb", { arg file;
							// plugins are seperated by newlines
							file.readAllString.split($\n).do { arg line;
								(line.size > 0).if {
									var info = this.prParseInfo(line);
									dict[info.key] = info;
								}
							};
						});
					} { arg error;
						error.notNil.if { "Failed to read tmp file!".warn };
						File.delete(filePath).not.if { ("Could not delete data file:" + filePath).warn };
						// done
						action.value;
					}
				} { "Failed to write tmp file!".warn };
			};
		}.forkIfNeeded;
	}
	*prSearchRemote { arg server, searchPaths, useDefault, verbose, wait, action;
		var cb, dict = pluginDict[server];
		cb = OSCFunc({ arg msg;
			var result = msg[1].asString.split($\n);
			(result[0] == "/vst_search").if {
				var fn, count = 0, num = result[1].asInteger;
				cb.free; // got correct /done message, free it!
				(num == 0).if {
					action.value;
					^this;
				};
				"probing plugins".post;
				fn = OSCFunc({ arg msg;
					var key, info, result = msg[1].asString.split($\n);
					(result[0].asSymbol == '/vst_info').if {
						key = result[1].asSymbol;
						info = this.prMakeInfo(key, result[2..]);
						dict[key] = info;
						count = count + 1;
						// exit condition
						(count == num).if {
							fn.free;
							this.prQueryPlugins(server, wait, {".".post}, {
								action.value;
								"probed % plugins.".format(num).postln;
								"done!".postln;
							});
						}
					}
				}, '/done');
				{
					num.do { arg index;
						server.sendMsg('/cmd', '/vst_query', index);
						".".post;
						if(wait >= 0) { wait.wait } { server.sync };
					}
				}.forkIfNeeded;
			};
		}, '/done');
		{
			server.sendMsg('/cmd', '/vst_path_clear');
			searchPaths.do { arg path;
				server.sendMsg('/cmd', '/vst_path_add', path);
				if(wait >= 0) { wait.wait } { server.sync }; // for safety
			};
			server.sendMsg('/cmd', '/vst_search', useDefault, verbose);
		}.forkIfNeeded;
	}
	*prParseInfo { arg string;
		var data, key, info, nparam, nprogram;
		// data is seperated by tabs
		data = string.split($\t);
		key = data[0].asSymbol;
		info = this.prMakeInfo(key, data[1..9]);
		// get parameters (name + label)
		nparam = info.numParameters;
		info.parameterNames = Array.new(nparam);
		info.parameterLabels = Array.new(nparam);
		nparam.do { arg i;
			var onset = 10 + (i * 2);
			info.parameterNames.add(data[onset]);
			info.parameterLabels.add(data[onset + 1]);
		};
		// get programs
		nprogram = info.numPrograms;
		info.programs = Array.new(nprogram);
		nprogram.do { arg i;
			var onset = 10 + (nparam * 2) + i;
			info.programs.add(data[onset]);
		};
		^info;
	}
	*probe { arg server, path, key, wait = -1, action;
		var cb;
		server = server ?? Server.default;
		// if key is nil, use the plugin path as key
		key = key.notNil.if { key.asSymbol } { path.asSymbol };
		// add dictionary if it doesn't exist yet
		pluginDict[server].isNil.if { pluginDict[server] = IdentityDictionary.new };
		server.isLocal.if { this.prProbeLocal(server, path, key, action); }
		{ this.prProbeRemote(server, path, key, wait, action); };
	}
	*prProbeLocal { arg server, path, key, action;
		{
			var info, filePath = PathName.tmp ++ this.hash.asString;
			// ask server to read tmp file and replace the content with the plugin info
			server.sendMsg('/cmd', '/vst_query', path, filePath);
			// wait for cmd to finish
			server.sync;
			// read file (only written if the plugin could be probed)
			File.exists(filePath).if {
				protect {
					File.use(filePath, "rb", { arg file;
						info = this.prParseInfo(file.readAllString);
						info.key = key; // overwrite
						pluginDict[server][key] = info;
					});
					"'%' successfully probed".format(key).postln;
				} { arg error;
					error.notNil.if { "Failed to read tmp file!".warn };
					File.delete(filePath).not.if { ("Could not delete data file:" + filePath).warn };
				}
			};
			// done
			action.value(info);
		}.forkIfNeeded;
	}
	*prProbeRemote { arg server, path, key, wait, action;
		var cb = OSCFunc({ arg msg;
			var result = msg[1].asString.split($\n);
			(result[0] == "/vst_info").if {
				cb.free; // got correct /done message, free it!
				(result.size > 1).if {
					var info = this.prMakeInfo(key, result[2..]);
					pluginDict[server][key] = info;
					this.prQueryPlugin(server, key, wait, { action.value(info) });
					"'%' successfully probed".format(key).postln;
				} { action.value };
			};
		}, '/done');
		server.sendMsg('/cmd', '/vst_query', path);
	}
	*prQueryPlugins { arg server, wait, update, action;
		var gen, fn, dict;
		dict = pluginDict[server];
		(dict.size == 0).if {
			action.value;
			^this;
		};
		// make a generator for plugin keys
		gen = Routine.new({
			dict.keysDo { arg key; yield(key) };
		});
		// recursively call the generator
		fn = { arg key;
			key.notNil.if {
				this.prQueryPlugin(server, key, wait, {
					update.value; // e.g. ".".postln;
					fn.value(gen.next);
				});
			} { action.value }; // done (no keys left)
		};
		// start it
		fn.value(gen.next);
	}
	*prQueryPlugin { arg server, key, wait, action;
		this.prQueryParameters(server, key, wait, {
			this.prQueryPrograms(server, key, wait, {
				action.value;
				// "plugin % queried".format(key).postln;
			});
		});
	}
	*prQueryParameters { arg server, key, wait, action;
		var count = 0, fn, num, info;
		info = pluginDict[server][key];
		num = info.numParameters;
		info.parameterNames = Array.new(num);
		info.parameterLabels = Array.new(num);
		(num == 0).if {
			action.value;
			^this;
		};
		fn = OSCFunc({ arg msg;
			var result = msg[1].asString.split($\n);
			// result.postln;
			((result[0].asSymbol == '/vst_param_info')
				&& (result[1].asSymbol == key)
			).if {
				var n = (result.size - 2).div(2);
				// "got % parameters for %".format(n, key).postln;
				n.do { arg i;
					var idx = (i * 2) + 2;
					info.parameterNames.add(result[idx].asSymbol);
					info.parameterLabels.add(result[idx+1].asSymbol);
				};
				count = count + n;
				// exit condition
				(count == num).if { fn.free; action.value };
			};
		}, '/done');
		this.prQuery(server, key, wait, num, '/vst_query_param');
	}
	*prQueryPrograms { arg server, key, wait, action;
		var count = 0, fn, num, info;
		info = pluginDict[server][key];
		num = info.numPrograms;
		info.programs = Array.new(num);
		(num == 0).if {
			action.value;
			^this;
		};
		fn = OSCFunc({ arg msg;
			var result = msg[1].asString.split($\n);
			// result.postln;
			((result[0].asSymbol == '/vst_program_info')
				&& (result[1].asSymbol == key)
			).if {
				var n = result.size - 2;
				// "got % programs for %".format(n, key).postln;
				n.do { arg i;
					info.programs.add(result[i + 2].asSymbol);
				};
				count = count + n;
				// exit condition
				(count == num).if { fn.free; action.value };
			};
		}, '/done');
		this.prQuery(server, key, wait, num, '/vst_query_program');
	}
	*prQuery { arg server, key, wait, num, cmd;
		{
			var div = num.div(16), mod = num.mod(16);
			// request 16 parameters/programs at once
			div.do { arg i;
				server.sendMsg('/cmd', cmd, key, i * 16, 16);
				if(wait >= 0) { wait.wait } { server.sync };
			};
			// request remaining parameters/programs
			(mod > 0).if { server.sendMsg('/cmd', cmd, key, num - mod, mod) };
		}.forkIfNeeded;
	}
	*prMakeInfo { arg key, info;
		var f, flags;
		f = info[8].asInt;
		flags = Array.fill(8, {arg i; ((f >> i) & 1).asBoolean });
		^(
			parent: parentInfo,
			key: key,
			name: info[0],
			path: info[1].asString,
			version: info[2].asInt,
			id: info[3].asInt,
			numInputs: info[4].asInt,
			numOutputs: info[5].asInt,
			numParameters: info[6].asInt,
			numPrograms: info[7].asInt,
			hasEditor: flags[0],
			isSynth: flags[1],
			singlePrecision: flags[2],
			doublePrecision: flags[3],
			midiInput: flags[4],
			midiOutput: flags[5],
			sysexInput: flags[6],
			sysexOutput: flags[7]
		);
	}
	*prGetInfo { arg server, key, wait, action;
		key = key.asSymbol;
		// if the key already exists, return the info, otherwise probe the plugin
		pluginDict[server] !? { arg dict; dict[key] } !? { arg info; action.value(info) }
		?? { this.probe(server, key, key, wait, action) };
	}
	// instance methods
	init { arg theID, numOut ... theInputs;
		id = theID;
		inputs = theInputs;
		^this.initOutputs(numOut, rate)
	}
}

VstPluginController {
	// constants
	const oscPacketSize = 1600; // safe max. OSC packet size
	// public
	var <synth;
	var <synthIndex;
	var <loaded;
	var <info;
	var <midi;
	var <programs;
	var <currentProgram;
	var <>wait;
	// callbacks
	var <>parameterAutomated;
	var <>midiReceived;
	var <>sysexReceived;
	// private
	var oscFuncs;
	var scGui;

	*new { arg synth, id, synthDef, wait= -1;
		var synthIndex;
		// if the synthDef is nil, we try to get it from the global SynthDescLib
		synthDef.isNil.if {
			var desc;
			desc = SynthDescLib.global.at(synth.defName);
			desc.isNil.if { ^"couldn't find synthDef in global SynthDescLib!".throw };
			synthDef = desc.def;
		};
		// walk the list of UGens and get the VstPlugin instance which matches the given ID.
		// if 'id' is nil, pick the first instance. throw an error if no VstPlugin is found.
		synthDef.children.do { arg ugen, index;
			(ugen.class == VstPlugin).if {
				(id.isNil || (ugen.id == id)).if {
					^super.new.init(synth, index, wait);
				}
			};
		};
		id.isNil.if {^"synth doesn't contain a VstPlugin!".throw;}
		{^"synth doesn't contain a VstPlugin with ID '%'".format(id).throw;}
	}
	init { arg theSynth, theIndex, waitTime;
		synth = theSynth;
		synthIndex = theIndex;
		wait = waitTime;
		loaded = false;
		midi = VstPluginMIDIProxy(this);
		oscFuncs = List.new;
		// parameter changed:
		oscFuncs.add(this.makeOSCFunc({ arg msg;
			var index, value, display;
			// we have to defer the method call to the AppClock!
			{scGui.notNil.if {
				index = msg[3].asInt;
				value = msg[4].asFloat;
				(msg.size > 5).if {
					display = this.class.msg2string(msg, 5);
				};
				scGui.paramChanged(index, value, display);
			}}.defer;
		}, '/vst_param'));
		// current program:
		oscFuncs.add(this.makeOSCFunc({ arg msg;
			currentProgram = msg[3].asInt;
		}, '/vst_program_index'));
		// program name:
		oscFuncs.add(this.makeOSCFunc({ arg msg;
			var index, name;
			index = msg[3].asInt;
			name = this.class.msg2string(msg, 4);
			programs[index] = name;
		}, '/vst_program'));
		// parameter automated:
		oscFuncs.add(this.makeOSCFunc({ arg msg;
			var index, value;
			index = msg[3].asInt;
			value = msg[4].asFloat;
			parameterAutomated.value(index, value);
		}, '/vst_auto'));
		// MIDI received:
		oscFuncs.add(this.makeOSCFunc({ arg msg;
			// convert to integers and pass as args to action
			midiReceived.value(*Int32Array.newFrom(msg[3..]));
		}, '/vst_midi'));
		// sysex received:
		oscFuncs.add(this.makeOSCFunc({ arg msg;
			// convert to Int8Array and pass to action
			sysexReceived.value(Int8Array.newFrom(msg[3..]));
		}, '/vst_sysex'));
		// cleanup after synth has been freed:
		synth.onFree { this.prFree };
	}
	prFree {
		// "VstPluginController: synth freed!".postln;
		oscFuncs.do { arg func;
			func.free;
		};
		this.prClear();
		this.prClearGui();
	}
	showGui { arg show=true;
		loaded.if {
			scGui.isNil.if { this.sendMsg('/vis', show.asInt)} { scGui.view.visible_(show)};
		}
	}
	open { arg path, gui=\sc, info=false, action;
		var theInfo;
		this.prClear();
		this.prClearGui();
		this.makeOSCFunc({arg msg;
			var success = msg[3].asBoolean;
			var window = msg[4].asBoolean;
			success.if {
				loaded = true;
				this.slotPut('info', theInfo); // hack because of name clash with 'info'
				currentProgram = 0;
				// copy default program names (might change later when loading banks)
				programs = Array.newFrom(theInfo.programs);
				// make SC editor if needed/wanted (deferred to AppClock!)
				window.not.if {
					{scGui = VstPluginGui.new(this)}.defer;
				};
				this.prQueryParams;
				// post info if wanted
				info.if { theInfo.print };
			};
			action.value(this, success);
		}, '/vst_open').oneShot;
		VstPlugin.prGetInfo(synth.server, path, wait, { arg i;
			// don't set 'info' property yet
			theInfo = i;
			// if no plugin info could be obtained (probing failed)
			// we open the plugin nevertheless to get some error messages
			this.sendMsg('/open', theInfo !? { theInfo.path } ?? path, (gui == \vst).asInt);
		});
	}
	prClear {
		loaded = false; info = nil;	programs = nil; currentProgram = nil;
	}
	prClearGui {
		scGui.notNil.if { {scGui.view.remove; scGui = nil}.defer };
	}
	close {
		this.sendMsg('/close');
		this.prClear();
		this.prClearGui();
	}
	reset { arg async = false;
		this.sendMsg('/reset', async.asInt);
	}
	// parameters
	numParameters {
		^(info !? (_.numParameters) ?? 0);
	}
	set { arg ...args;
		this.sendMsg('/set', *args);
	}
	setn { arg ...args;
		var nargs = List.new;
		args.pairsDo { arg index, values;
			values.isArray.if {
				nargs.addAll([index, values.size]++ values);
			} {
				nargs.addAll([index, 1, values]);
			};
		};
		this.sendMsg('/setn', *nargs);
	}
	get { arg index, action;
		this.makeOSCFunc({ arg msg;
			// msg: address, nodeID, index, value
			action.value(msg[3]); // only pass value
		}, '/vst_set').oneShot;
		this.sendMsg('/get', index);
	}
	getn { arg index = 0, count = -1, action;
		(count < 0).if { count = this.numParameters - index };
		this.makeOSCFunc({ arg msg;
			// msg: address, nodeID, index, count, values...
			action.value(msg[4..]); // only pass values
		}, '/vst_setn').oneShot;
		this.sendMsg('/getn', index, count);
	}
	map { arg ...args;
		var nargs = List.new;
		args.pairsDo { arg index, bus;
			bus = bus.asBus;
			(bus.rate == \control).if {
				nargs.addAll([index, bus.index, bus.numChannels]);
			} {
				^"bus must be control rate!".throw;
			}
		};
		this.sendMsg('/map', *nargs);
	}
	unmap { arg ...args;
		this.sendMsg('/unmap', *args);
	}
	// programs and banks
	numPrograms {
		^(info !? (_.numPrograms) ?? 0);
	}
	setProgram { arg index;
		((index >= 0) && (index < this.numPrograms)).if {
			{
				this.sendMsg('/program_set', index);
				// wait one roundtrip for async command to finish
				synth.server.sync;
				this.prQueryParams;
			}.forkIfNeeded;
		} {
			^"program number % out of range".format(index).throw;
		};
	}
	setProgramName { arg name;
		this.sendMsg('/program_name', name);
	}
	readProgram { arg path, action;
		this.makeOSCFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
			this.prQueryParams;
		}, '/vst_program_read').oneShot;
		this.sendMsg('/program_read', path);
	}
	readBank { arg path, action;
		this.makeOSCFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
			this.prQueryParams;
			this.prQueryPrograms;
		}, '/vst_bank_read').oneShot;
		this.sendMsg('/bank_read', path);
	}
	writeProgram { arg path, action;
		this.makeOSCFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
		}, '/vst_program_write').oneShot;
		this.sendMsg('/program_write', path);
	}
	writeBank { arg path, action;
		this.makeOSCFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
		}, '/vst_bank_write').oneShot;
		this.sendMsg('/bank_write', path);
	}
	setProgramData { arg data, action;
		(data.class != Int8Array).if {^"'%' expects Int8Array!".format(thisMethod.name).throw};
		synth.server.isLocal.if {
			this.prSetData(data, action, false);
		} { "'%' only works with a local Server".format(thisMethod.name).warn; ^nil };
	}
	setBankData { arg data, action;
		(data.class != Int8Array).if {^"'%' expects Int8Array!".format(thisMethod.name).throw};
		synth.server.isLocal.if {
			this.prSetData(data, action, true);
		} { "'%' only works with a local Server".format(thisMethod.name).warn; ^nil };
	}
	prSetData { arg data, action, bank;
		var path, cb;
		path = PathName.tmp ++ this.hash.asString;
		cb = { arg self, success;
			success.if {
				File.delete(path).not.if { ("Could not delete data file:" + path).warn };
			};
			action.value(self, success);
		};
		protect {
			File.use(path, "wb", { arg file; file.write(data) });
		} { arg error;
			error.isNil.if {
				bank.if { this.readBank(path, cb) } { this.readProgram(path, cb) };
			} { "Failed to write data".warn };
		};
	}
	sendProgramData { arg data, wait, action;
		wait = wait ?? this.wait;
		(data.class != Int8Array).if {^"'%' expects Int8Array!".format(thisMethod.name).throw};
		this.prSendData(data, wait, action, false);
	}
	sendBankData { arg data, wait, action;
		wait = wait ?? this.wait;
		(data.class != Int8Array).if {^"'%' expects Int8Array!".format(thisMethod.name).throw};
		this.prSendData(data, wait, action, true);
	}
	prSendData { arg data, wait, action, bank;
		// split data into smaller packets and send them to the plugin.
		// wait = -1 allows an OSC roundtrip between packets.
		// wait = 0 might not be safe in a high traffic situation,
		// maybe okay with tcp.
		var totalSize, address, resp, sym, pos = 0;
		loaded.not.if {"can't send data - no plugin loaded!".warn; ^nil };
		sym = bank.if {'bank' } {'program'};
		address = "/"++sym++"_data_set";
		totalSize = data.size;

		resp = this.makeOSCFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
			this.prQueryParams(wait);
			bank.if { this.prQueryPrograms(wait) };
		}, "/vst_"++sym++"_read").oneShot;

		{
			while { pos < totalSize } {
				var end = pos + oscPacketSize - 1; // 'end' can safely exceed 'totalSize'
				this.sendMsg(address, totalSize, pos, data[pos..end]);
				pos = pos + oscPacketSize;
				(wait >= 0).if { wait.wait } { synth.server.sync };
			};
		}.forkIfNeeded
	}
	getProgramData { arg action;
		this.prGetData(action, false);
	}
	getBankData { arg action;
		this.prGetData(action, true);
	}
	prGetData { arg action, bank;
		var path, cb, data;
		path = PathName.tmp ++ this.hash.asString;
		cb = { arg self, success;
			success.if {
				protect {
					File.use(path, "rb", { arg file;
						data = Int8Array.newClear(file.length);
						file.read(data);
					});
				} { arg error;
					error.notNil.if { "Failed to read data".warn };
					File.delete(path).not.if { ("Could not delete data file:" + path).warn };
				};
			} { "Could not get data".warn };
			action.value(data);
		};
		bank.if { this.writeBank(path, cb) } { this.writeProgram(path, cb) };
	}
	receiveProgramData { arg wait=0.01, timeout=3, action;
		this.prReceiveData(wait, timeout, action, false);
	}
	receiveBankData { arg wait=0.01, timeout=3, action;
		this.prReceiveData(wait, timeout, action, true);
	}
	prReceiveData { arg wait, timeout, action, bank;
		// get data in smaller packets and assemble them.
		// wait = -1 allows an OSC roundtrip between packets.
		// wait = 0 might not be safe in a high traffic situation,
		// maybe okay with tcp.
		var data, resp, address, sym, count = 0, done = false;
		wait = wait ?? this.wait;
		loaded.not.if {"can't receive data - no plugin loaded!".warn; ^nil };
		sym = bank.if {'bank' } {'program'};
		address = "/"++sym++"_data_get";
		data = Int8Array.new;

		resp = this.makeOSCFunc({ arg msg;
			var total, onset, size;
			total = msg[3].asInt;
			onset = msg[4].asInt;
			size = msg[5].asInt;
			// allocate array on the first packet
			(onset == 0).if { data = Int8Array.new(total) };
			// add packet
			data.addAll(msg[6..]);
			// send when done
			(data.size >= total).if {
				done = true;
				resp.free;
				action.value(data);
			};
		}, "/vst_"++sym++"_data");

		{
			while { done.not } {
				this.sendMsg(address, count);
				count = count + 1;
				if(wait >= 0) { wait.wait } { synth.server.sync };
			};

		}.forkIfNeeded;

		// lose the responder if the network choked
		SystemClock.sched(timeout, {
			if(done.not) {
				resp.free;
				"Receiving data failed!".warn;
				"Try increasing wait time".postln
			}
		});
	}
	// MIDI / Sysex
	sendMidi { arg status, data1=0, data2=0;
		this.sendMsg('/midi_msg', Int8Array.with(status, data1, data2));
	}
	sendSysex { arg msg;
		(msg.class != Int8Array).if {^"'%' expects Int8Array!".format(thisMethod.name).throw};
		(msg.size > oscPacketSize).if {
			"sending sysex data larger than % bytes is risky".format(oscPacketSize).warn;
		};
		this.sendMsg('/midi_sysex', msg);
	}
	// transport
	setTempo { arg bpm=120;
		this.sendMsg('/tempo', bpm);
	}
	setTimeSignature { arg num=4, denom=4;
		this.sendMsg('/time_sig', num, denom);
	}
	setPlaying { arg b;
		this.sendMsg('/transport_play', b.asInt);
	}
	setTransportPos { arg pos;
		this.sendMsg('/transport_set', pos);
	}
	getTransportPos { arg action;
		this.makeOSCFunc({ arg msg;
			action.value(msg[3]);
		}, '/vst_transport').oneShot;
		this.sendMsg('/transport_get');
	}
	// advanced
	canDo { arg what, action;
		this.makeOSCFunc({ arg msg;
			action.value(msg[3].asInt);
		}, '/vst_can_do').oneShot;
		this.sendMsg('/can_do', what);
	}
	vendorMethod { arg index=0, value=0, ptr, opt=0.0, action=0;
		this.makeOSCFunc({ arg msg;
			action.value(msg[3].asInt);
		}, '/vst_vendor_method').oneShot;
		ptr = ptr ?? Int8Array.new;
		this.sendMsg('/vendor_method', index, value, ptr, opt);
	}
	// internal
	sendMsg { arg cmd ... args;
		synth.server.sendMsg('/u_cmd', synth.nodeID, synthIndex, cmd, *args);
	}
	makeOSCFunc { arg func, path;
		^OSCFunc(func, path, synth.server.addr, argTemplate: [synth.nodeID, synthIndex]);
	}
	*msg2string { arg msg, onset=0;
		// format: len, chars...
		var len = msg[onset].asInt;
		(len > 0).if {
			^msg[(onset+1)..(onset+len)].collectAs({arg item; item.asInt.asAscii}, String);
		} { ^"" };
	}
	prQueryParams { arg wait;
		this.prQuery(wait, this.numParameters, '/param_query');
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
VstPluginMIDIProxy {
	var <>latency = 0;
	var <>port = 0;
	var <>uid = 0;
	var <>owner;

	*new { arg theOwner;
		^super.new.owner_(theOwner);
	}
	write { arg len, hiStatus, loStatus, a=0, b=0;
		owner.sendMidi(hiStatus bitOr: loStatus, a, b);
	}
	noteOn { arg chan, note=60, veloc=64;
		this.write(3, 16r90, chan.asInteger, note.asInteger, veloc.asInteger);
	}
	noteOff { arg chan, note=60, veloc=64;
		this.write(3, 16r80, chan.asInteger, note.asInteger, veloc.asInteger);
	}
	polyTouch { arg chan, note=60, val=64;
		this.write(3, 16rA0, chan.asInteger, note.asInteger, val.asInteger);
	}
	control { arg chan, ctlNum=7, val=64;
		this.write(3, 16rB0, chan.asInteger, ctlNum.asInteger, val.asInteger);
	}
	program { arg chan, num=1;
		this.write(2, 16rC0, chan.asInteger, num.asInteger);
	}
	touch { arg chan, val=64;
		this.write(2, 16rD0, chan.asInteger, val.asInteger);
	}
	bend { arg chan, val=8192;
		val = val.asInteger;
		this.write(3, 16rE0, chan, val bitAnd: 127, val >> 7);
	}
	allNotesOff { arg chan;
		this.control(chan, 123, 0);
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
	songPtr { arg songPtr;
		songPtr = songPtr.asInteger;
		this.write(4, 16rF0, 16r02, songPtr & 16r7f, songPtr >> 7 & 16r7f);
	}
	songSelect { arg song;
		this.write(3, 16rF0, 16r03, song.asInteger);
	}
	midiClock {
		this.write(1, 16rF0, 16r08);
	}
	start {
		this.write(1, 16rF0, 16r0A);
	}
	continue {
		this.write(1, 16rF0, 16r0B);
	}
	stop {
		this.write(1, 16rF0, 16r0C);
	}
	reset {
		this.write(1, 16rF0, 16r0F);
	}
	sysex { arg packet;
		owner.sendSysex(packet);
	}
}

VstPluginGui {
	classvar <>maxParams = 12; // max. number of parameters per column
	classvar <>sliderWidth = 180;
	classvar <>sliderHeight = 20;
	classvar <>displayWidth = 60;
	// public
	var <view;
	// private
	var paramSliders;
	var paramDisplays;
	var model;

	*new { arg model;
		^super.new.init(model);
	}

	init { arg theModel;
		var nparams, title, ncolumns, nrows, bounds, layout, font;

		model = theModel;

		nparams = model.numParameters;
		ncolumns = nparams.div(maxParams) + ((nparams % maxParams) != 0).asInt;
		(ncolumns == 0).if {ncolumns = 1}; // just to prevent division by zero
		nrows = nparams.div(ncolumns) + ((nparams % ncolumns) != 0).asInt;

		font = Font.new(*GUI.skin.fontSpecs);
		bounds = Rect(0,0, sliderWidth * ncolumns, sliderHeight * nrows);
		// don't delete the view on closing the window!
		view = View.new(nil, bounds).name_(model.info.name).deleteOnClose_(false);

		title = StaticText.new()
			.stringColor_(GUI.skin.fontColor)
			.font_(font)
			.background_(GUI.skin.background)
			.align_(\left)
			.object_(model.info.name);

		layout = GridLayout.new();
		layout.add(title, 0, 0);

		paramSliders = List.new();
		paramDisplays = List.new();

		nparams.do { arg i;
			var col, row, name, label, display, slider, unit;
			col = i.div(nrows);
			row = i % nrows;
			name = StaticText.new.string_("%: %".format(i, model.info.parameterNames[i]));
			label = StaticText.new.string_(model.info.parameterLabels[i] ?? "");
			display = TextField.new.fixedWidth_(displayWidth);
			display.action = {arg s; model.set(i, s.value)};
			paramDisplays.add(display);
			slider = Slider.new(bounds: sliderWidth@sliderHeight).fixedHeight_(sliderHeight);
			slider.action = {arg s; model.set(i, s.value)};
			paramSliders.add(slider);
			unit = VLayout.new(
				HLayout.new(name.align_(\left), nil, display.align_(\right), label.align_(\right)),
				slider;
			);
			layout.add(unit, row+1, col);
		};
		layout.setRowStretch(nparams+1, 1);

		view.layout_(layout);
	}

	paramChanged { arg index, value, display;
		paramSliders.at(index).value = value;
		paramDisplays.at(index).string = display;
	}
}

