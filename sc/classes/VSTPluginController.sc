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
			paramCache[index] = [value, display];
			// notify dependants
			this.changed('/param', index, value, display);
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
			programNames[index] = name;
			// notify dependants
			this.changed('/program', index, name);
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
	gui { arg parent, bounds;
		^this.class.guiClass.new(this).gui(parent, bounds);
	}
	open { arg path, editor=false, info=false, action;
		// if path is nil we try to get it from VSTPlugin
		path ?? {
			this.info !? { path = this.info.key } ?? { ^"'path' is nil but VSTPlugin doesn't have a plugin info".error }
		};
		VSTPlugin.prGetInfo(synth.server, path, wait, { arg theInfo;
			theInfo.notNil.if {
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
							programNames = theInfo.programs.collect(_.name);
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
				// don't set 'info' property yet
				this.sendMsg('/open', theInfo.key, editor.asInteger);
			} { "couldn't open '%'".format(path).error; };
		});
	}
	openMsg { arg path, editor=false;
		// path must be a plugin name or *absolute* file path.
		// (we can't really distinguish between relative file paths and plugin names
		// without having access to the plugin dictionary)
		^this.makeMsg('/open', path, editor.asInteger);
	}
	prClear {
		loaded = false; window = false; info = nil;	paramCache = nil; programNames = nil; program = nil;
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
		(count < 0).if { count = this.numParameters - index };
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
	readProgram { arg path, action;
		path = VSTPlugin.prResolvePath(path);
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
			this.prQueryParams;
		}, '/vst_program_read').oneShot;
		this.sendMsg('/program_read', path);
	}
	readProgramMsg { arg dest;
		^this.makeMsg('/program_read', VSTPlugin.prMakeDest(dest));
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
	readBankMsg { arg dest;
		^this.makeMsg('/bank_read', VSTPlugin.prMakeDest(dest));
	}
	writeProgram { arg path, action;
		path = VSTPlugin.prResolvePath(path);
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
		}, '/vst_program_write').oneShot;
		this.sendMsg('/program_write', path);
	}
	writeProgramMsg { arg dest;
		^this.makeMsg('/program_write', VSTPlugin.prMakeDest(dest));
	}
	writeBank { arg path, action;
		path = VSTPlugin.prResolvePath(path);
		this.prMakeOscFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
		}, '/vst_bank_write').oneShot;
		this.sendMsg('/bank_write', path);
	}
	writeBankMsg { arg dest;
		^this.makeMsg('/bank_write', VSTPlugin.prMakeDest(dest));
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
			bank.if { this.readBank(path, cb) } { this.readProgram(path, cb) };
		} { "Failed to write data".warn };
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
			this.sendMsg("/"++sym++"_read", VSTPlugin.prMakeDest(buf));
		});
	}
	getProgramData { arg action;
		this.prGetData(action, false);
	}
	getBankData { arg action;
		this.prGetData(action, true);
	}
	prGetData { arg action, bank;
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
				} { "Failed to read data".warn };
				File.delete(path).not.if { ("Could not delete data file:" + path).warn };
			} { "Could not get data".warn };
			// done (on fail, data is nil)
			action.value(data);
		};
		// 1) ask plugin to write data file
		bank.if { this.writeBank(path, cb) } { this.writeProgram(path, cb) };
	}
	receiveProgramData { arg wait, timeout=3, action;
		this.prReceiveData(wait, timeout, action, false);
	}
	receiveBankData { arg wait, timeout=3, action;
		this.prReceiveData(wait, timeout, action, true);
	}
	prReceiveData { arg wait, timeout, action, bank;
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
			this.sendMsg("/"++sym++"_write", VSTPlugin.prMakeDest(buf));
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
		^this.mskeMsg('/tempo', bpm);
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
	write { arg len, hiStatus, loStatus, a=0, b=0, detune;
		owner.sendMidi(hiStatus bitOr: loStatus, a, b, detune);
	}
	writeMsg { arg len, hiStatus, loStatus, a=0, b=0, detune;
		^owner.sendMidiMsg(hiStatus bitOr: loStatus, a, b, detune);
	}
	noteOn { arg chan, note=60, veloc=64, detune;
		this.write(3, 16r90, chan.asInteger, note.asInteger, veloc.asInteger, detune);
	}
	noteOnMsg { arg chan, note=60, veloc=64, detune;
		^this.writeMsg(3, 16r90, chan.asInteger, note.asInteger, veloc.asInteger, detune);
	}
	noteOff { arg chan, note=60, veloc=64;
		this.write(3, 16r80, chan.asInteger, note.asInteger, veloc.asInteger);
	}
	noteOffMsg { arg chan, note=60, veloc=64;
		^this.writeMsg(3, 16r80, chan.asInteger, note.asInteger, veloc.asInteger);
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
