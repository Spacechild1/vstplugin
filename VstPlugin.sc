VstPlugin : MultiOutUGen {
	var <id;
	*ar { arg input, numOut=1, bypass=0, params, id;
		var numIn = 0;
		input.notNil.if {
			numIn = input.isArray.if {input.size} { 1 };
		}
		^this.multiNewList([\audio, id, numOut, bypass, numIn] ++ input ++ params);
	}
	*kr { ^this.shouldNotImplement(thisMethod) }
	init { arg theID, numOut ... theInputs;
		id = theID;
		inputs = theInputs;
		^this.initOutputs(numOut, rate)
	}
}

VstPluginController {
	// flags
	const fHasEditor = 1;
	const fIsSynth = 2;
	const fSinglePrecision = 4;
	const fDoublePrecision = 8;
	const fMidiInput = 16;
	const fMidiOutput = 32;
	// constants
	const oscPacketSize = 1600; // safe max. OSC packet size

	// public
	var <synth;
	var <synthIndex;
	var <loaded;
	var <name;
	var <version;
	var <hasEditor;
	var <isSynth;
	var <numInputs;
	var <numOutputs;
	var <singlePrecision;
	var <doublePrecision;
	var <midiInput;
	var <midiOutput;
	var <programs;
	var <currentProgram;
	var <parameterNames;
	var <parameterLabels;
	// callbacks
	var <>parameterAutomated;
	var <>midiReceived;
	var <>sysexReceived;
	// private
	var oscFuncs;
	var scGui;
	var notify;

	*new { arg synth, id, synthDef;
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
					^super.new.init(synth, id, synthDef, index);
				}
			};
		};
		id.isNil.if {^"synth doesn't contain a VstPlugin!".throw;}
		{^"synth doesn't contain a VstPlugin with ID '%'".format(id).throw;}
	}
	init { arg theSynth, id, synthDef, theIndex;
		synth = theSynth;
		synthIndex = theIndex;
		loaded = false;
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
		}, '/vst_p'));
		// parameter automated:
		oscFuncs.add(this.makeOSCFunc({ arg msg;
			var index, value;
			parameterAutomated.notNil.if {
				index = msg[3].asInt;
				value = msg[4].asFloat;
				parameterAutomated.value(index, value);
			}
		}, '/vst_pa'));
		// parameter name + label:
		oscFuncs.add(this.makeOSCFunc({ arg msg;
			var index, name, label;
			index = msg[3].asInt;
			name = this.class.msg2string(msg, 4);
			label = this.class.msg2string(msg, 4 + name.size + 1);
			parameterNames[index] = name;
			parameterLabels[index] = label;
		}, '/vst_pn'));
		// current program:
		oscFuncs.add(this.makeOSCFunc({ arg msg;
			currentProgram = msg[3].asInt;
		}, '/vst_pgm'));
		// program name:
		oscFuncs.add(this.makeOSCFunc({ arg msg;
			var index, name;
			index = msg[3].asInt;
			name = this.class.msg2string(msg, 4);
			programs[index] = name;
		}, '/vst_pgmn'));
		// MIDI received:
		oscFuncs.add(this.makeOSCFunc({ arg msg;
			midiReceived.notNil.if {
				// convert to integers and pass as args to action
				midiReceived.value(*Int32Array.newFrom(msg[3..]));
			}
		}, '/vst_midi'));
		// sysex received:
		oscFuncs.add(this.makeOSCFunc({ arg msg;
			sysexReceived.notNil.if {
				// convert to Int8Array and pass to action
				sysexReceived.value(Int8Array.newFrom(msg[3..]));
			}
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
		synth = nil;
		synthIndex = nil;
	}
	showGui { arg show=true;
		loaded.if {
			scGui.isNil.if { this.sendMsg('/vis', show.asInt)} { scGui.view.visible_(show)};
		}
	}
	info {
		loaded.if {
			"---".postln;
			"name: '%'".format(name).postln;
			"version: %".format(version).postln;
			"editor: %".format(hasEditor).postln;
			"input channels: %".format(numInputs).postln;
			"output channels: %".format(numOutputs).postln;
			"single precision: %".format(singlePrecision).postln;
			"double precision: %".format(doublePrecision).postln;
			"MIDI input: %".format(midiInput).postln;
			"MIDI output: %".format(midiOutput).postln;
			"synth: %".format(isSynth).postln;
			"".postln;
			"parameters (%):".format(this.numParameters).postln;
			this.numParameters.do { arg i;
				var label;
				label = (parameterLabels[i].size > 0).if { "(%)".format(parameterLabels[i]) };
				"[%] % %".format(i, parameterNames[i], label ?? "").postln;
			};
			"".postln;
			"programs (%):".format(programs.size).postln;
			programs.do { arg item, i;
				"[%] %".format(i, item).postln;
			};
		} {
			"---".postln;
			"no plugin loaded!".postln;
		}
	}
	open { arg path, onSuccess, onFail, gui=\sc, info=false;
		var guiType;
		this.prClear();
		this.prClearGui();
		// the UGen will respond to the '/open' message with the following messages:
		this.makeOSCFunc({arg msg;
			loaded = true;
			name = this.class.msg2string(msg, 3)
		}, '/vst_name').oneShot;
		this.makeOSCFunc({arg msg;
			var nparam, npgm, flags;
			numInputs = msg[3].asInt;
			numOutputs = msg[4].asInt;
			nparam = msg[5].asInt;
			npgm = msg[6].asInt;
			flags = msg[7].asInt;
			version = msg[8].asInt;
			parameterNames = Array.fill(nparam, nil);
			parameterLabels = Array.fill(nparam, nil);
			programs = Array.fill(npgm, nil);
			hasEditor = (flags & fHasEditor).asBoolean;
			isSynth = (flags & fIsSynth).asBoolean;
			singlePrecision = (flags & fSinglePrecision).asBoolean;
			doublePrecision = (flags & fDoublePrecision).asBoolean;
			midiInput = (flags & fMidiInput).asBoolean;
			midiOutput = (flags & fMidiOutput).asBoolean;
		}, '/vst_info').oneShot;
		/* the /vst_open message is sent after /vst_name and /vst_info
		  and after parameter names and program names but *before*
		  parameter values are sent (so the GUI has a chance to respond to it) */
		this.makeOSCFunc({arg msg;
			msg[3].asBoolean.if {
				// make SC editor if needed/wanted (deferred to AppClock!)
				(gui == \sc || (gui == \vst && hasEditor.not)).if {
					{scGui = VstPluginGui.new(this)}.defer;
				};
				// print info if wanted
				info.if { this.info };
				onSuccess.value(this);
			} {
				onFail.value(this);
			};
		}, '/vst_open').oneShot;
		guiType = switch(gui)
			{\sc} {1}
			{\vst} {2}
			{0};
		this.sendMsg('/open', guiType, path);
	}
	prClear {
		loaded = false; name = nil; version = nil; numInputs = nil; numOutputs = nil;
		singlePrecision = nil; doublePrecision = nil; hasEditor = nil;
		midiInput = nil; midiOutput = nil; isSynth = nil;
		parameterNames = nil; parameterLabels = nil;
		programs = nil; currentProgram = nil;
	}
	prClearGui {
		scGui.notNil.if { {scGui.view.remove; scGui = nil}.defer };
	}
	close {
		this.sendMsg('/close');
		this.prClear();
		this.prClearGui();
	}
	reset { arg nrt = false;
		this.sendMsg('/reset', nrt.asInt);
	}
	// parameters
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
	numParameters {
		^parameterNames.size;
	}
	prNotify { arg b;
		b = b.asBoolean;
		notify = b;
		this.sendMsg('/notify', b.asInt);
	}
	// programs and banks
	numPrograms {
		^programs.size;
	}
	setProgram { arg index;
		((index >= 0) && (index < this.numPrograms)).if {
			this.sendMsg('/program_set', index);
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
		}, '/vst_program_read').oneShot;
		this.sendMsg('/program_read', path);
	}
	readBank { arg path, action;
		this.makeOSCFunc({ arg msg;
			var success = msg[3].asBoolean;
			action.value(this, success);
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
	sendProgramData { arg data, wait= -1, action;
		(data.class != Int8Array).if {^"'%' expects Int8Array!".format(thisMethod.name).throw};
		this.prSendData(data, wait, action, false);
	}
	sendBankData { arg data, wait= -1, action;
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
	// midi
	midiNoteOff { arg chan, note=60, veloc=64;
		this.prMidiMsg(128, chan, note, veloc);
	}
	midiNoteOn { arg chan, note=60, veloc=64;
		this.prMidiMsg(144, chan, note, veloc);
	}
	midiPolyTouch { arg chan, note=60, val=64;
		this.prMidiMsg(160, chan, note, val);
	}
	midiControl { arg chan, ctlNum=60, val=64;
		this.prMidiMsg(176, chan, ctlNum, val);
	}
	midiProgram { arg chan, num=1;
		this.prMidiMsg(192, chan, num);
	}
	midiTouch { arg chan, val=64;
		this.prMidiMsg(208, chan, val);
	}
	midiBend { arg chan, val=8192;
		var msb, lsb;
		val = val.asInt.clip(0, 16383);
		lsb = val & 127;
		msb = (val >> 7) & 127;
		this.prMidiMsg(224, chan, lsb, msb);
	}
	midiMsg { arg status, data1=0, data2=0;
		this.sendMsg('/midi_msg', Int8Array.with(status, data1, data2));
	}
	midiSysex { arg msg;
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
	prMidiMsg { arg hiStatus, lowStatus, data1=0, data2=0;
		var status = hiStatus.asInt + lowStatus.asInt.clip(0, 15);
		this.midiMsg(status, data1, data2);
	}
	*msg2string { arg msg, onset=0;
		// format: len, chars...
		var len = msg[onset].asInt;
		(len > 0).if {
			^msg[(onset+1)..(onset+len)].collectAs({arg item; item.asInt.asAscii}, String);
		} { ^"" };
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
		view = View.new(nil, bounds).name_(model.name).deleteOnClose_(false);

		title = StaticText.new()
			.stringColor_(GUI.skin.fontColor)
			.font_(font)
			.background_(GUI.skin.background)
			.align_(\left)
			.object_(model.name);

		layout = GridLayout.new();
		layout.add(title, 0, 0);

		paramSliders = List.new();
		paramDisplays = List.new();

		nparams.do { arg i;
			var col, row, name, label, display, slider, unit;
			col = i.div(nrows);
			row = i % nrows;
			name = StaticText.new.string_("%: %".format(i, model.parameterNames[i]));
			label = StaticText.new.string_(model.parameterLabels[i] ?? "");
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

