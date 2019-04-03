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
	var <programNames;
	var window;

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
					^super.new.init(synth, index, wait);
				}
			};
		};
		id.isNil.if {^"synth doesn't contain a VSTPlugin!".throw;}
		{^"synth doesn't contain a VSTPlugin with ID '%'".format(id).throw;}
	}
	init { arg theSynth, theIndex, waitTime;
		synth = theSynth;
		synthIndex = theIndex;
		wait = waitTime;
		loaded = false;
		midi = VSTPluginMIDIProxy(this);
		oscFuncs = List.new;
		// parameter changed:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			var index, value, display;
			index = msg[3].asInt;
			value = msg[4].asFloat;
			(msg.size > 5).if {
				display = this.class.msg2string(msg, 5);
			};
			// cache parameter value
			paramCache[index] = [value, display];
			// notify dependants
			this.changed('/param', index, value, display);
		}, '/vst_param'));
		// current program:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			program = msg[3].asInt;
			// notify dependants
			this.changed('/program_index', program);
		}, '/vst_program_index'));
		// program name:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			var index, name;
			index = msg[3].asInt;
			name = this.class.msg2string(msg, 4);
			programNames[index] = name;
			// notify dependants
			this.changed('/program', index, name);
		}, '/vst_program'));
		// parameter automated:
		oscFuncs.add(this.prMakeOscFunc({ arg msg;
			var index, value;
			index = msg[3].asInt;
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
		oscFuncs.do { arg func;
			func.free;
		};
		this.prClear;
		this.changed('/free');
	}
	editor { arg show=true;
		window.if { this.sendMsg('/vis', show.asInt); }
		{ "no editor!".postln; };
	}
	gui { arg parent, bounds;
		^this.class.guiClass.new(this).gui(parent, bounds);
	}
	open { arg path, editor=false, info=false, action;
		var theInfo;
		path.isNil.if {^this};
		this.prClear;
		this.prMakeOscFunc({arg msg;
			loaded = msg[3].asBoolean;
			window = msg[4].asBoolean;
			loaded.if {
				theInfo.notNil.if {
					this.slotPut('info', theInfo); // hack because of name clash with 'info'
					paramCache = Array.fill(theInfo.numParameters, [0, nil]);
					program = 0;
					// copy default program names (might change later when loading banks)
					programNames = Array.newFrom(theInfo.programNames);
					this.prQueryParams;
					// post info if wanted
					info.if { theInfo.print };
				} {
					// shouldn't happen...
					"bug: got no info!".error;
					loaded = false; window = false;
				};
			} {
				// shouldn't happen because /open is only sent if the plugin has been successfully probed
				"bug: couldn't open '%'".format(path).error;
			};
			action.value(this, loaded);
			this.changed('/open', path, loaded);
		}, '/vst_open').oneShot;
		VSTPlugin.prGetInfo(synth.server, path, wait, { arg i, resPath;
			// don't set 'info' property yet
			theInfo = i;
			theInfo.notNil.if { this.sendMsg('/open', theInfo.path, editor.asInt); }
			{ "couldn't open '%'".format(path).error; };
		});
	}
	prClear {
		loaded = false; window = false; info = nil;	paramCache = nil; programNames = nil; program = nil;
	}
	close {
		this.sendMsg('/close');
		this.prClear;
		this.changed('/close');
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
		this.prMakeOscFunc({ arg msg;
			// msg: address, nodeID, index, value
			action.value(msg[3]); // only pass value
		}, '/vst_set').oneShot;
		this.sendMsg('/get', index);
	}
	getn { arg index = 0, count = -1, action;
		(count < 0).if { count = this.numParameters - index };
		this.prMakeOscFunc({ arg msg;
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
	program_ { arg number;
		((number >= 0) && (number < this.numPrograms)).if {
			{
				this.sendMsg('/program_set', number);
				// wait one roundtrip for async command to finish
				synth.server.sync;
				this.prQueryParams;
			}.forkIfNeeded;
		} {
			^"program number % out of range".format(number).throw;
		};
	}
	programName {
		this.program.notNil.if {
			^programNames[this.program];
		} { ^nil };
	}
	programName_ { arg name;
		this.sendMsg('/program_name', name);
	}
	readProgram { arg path, action;
		path = VSTPlugin.prResolvePath(path);
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
			this.prQueryParams;
		}, '/vst_program_read').oneShot;
		this.sendMsg('/program_read', path);
	}
	readBank { arg path, action;
		path = VSTPlugin.prResolvePath(path);
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
			this.prQueryParams;
			this.prQueryPrograms;
		}, '/vst_bank_read').oneShot;
		this.sendMsg('/bank_read', path);
	}
	writeProgram { arg path, action;
		path = VSTPlugin.prResolvePath(path);
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
		}, '/vst_program_write').oneShot;
		this.sendMsg('/program_write', path);
	}
	writeBank { arg path, action;
		path = VSTPlugin.prResolvePath(path);
		this.prMakeOscFunc({ arg msg;
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
		var totalSize, address, resp, sym, end, pos = 0;
		wait = wait ?? this.wait;
		loaded.not.if {"can't send data - no plugin loaded!".warn; ^nil };
		sym = bank.if {'bank' } {'program'};
		address = "/"++sym++"_data_set";
		totalSize = data.size;

		resp = this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
			this.prQueryParams(wait);
			bank.if { this.prQueryPrograms(wait) };
		}, "/vst_"++sym++"_read").oneShot;

		{
			while { pos < totalSize } {
				end = pos + oscPacketSize - 1; // 'end' can safely exceed 'totalSize'
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
	receiveProgramData { arg wait, timeout=3, action;
		this.prReceiveData(wait, timeout, action, false);
	}
	receiveBankData { arg wait, timeout=3, action;
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

		resp = this.prMakeOscFunc({ arg msg;
			var total, onset, size;
			total = msg[3].asInt;
			onset = msg[4].asInt;
			size = msg[5].asInt;
			// allocate array on the first packet
			(onset == 0).if { data = Int8Array.new(total) };
			// add packet
			data = data.addAll(msg[6..]);
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
		this.prMakeOscFunc({ arg msg;
			action.value(msg[3]);
		}, '/vst_transport').oneShot;
		this.sendMsg('/transport_get');
	}
	// advanced
	canDo { arg what, action;
		this.prMakeOscFunc({ arg msg;
			action.value(msg[3].asInt);
		}, '/vst_can_do').oneShot;
		this.sendMsg('/can_do', what);
	}
	vendorMethod { arg index=0, value=0, ptr, opt=0.0, action=0;
		this.prMakeOscFunc({ arg msg;
			action.value(msg[3].asInt);
		}, '/vst_vendor_method').oneShot;
		ptr = ptr ?? Int8Array.new;
		this.sendMsg('/vendor_method', index, value, ptr, opt);
	}
	// internal
	sendMsg { arg cmd ... args;
		synth.server.sendMsg('/u_cmd', synth.nodeID, synthIndex, cmd, *args);
	}
	prMakeOscFunc { arg func, path;
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
VSTPluginMIDIProxy {
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
