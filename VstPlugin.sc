VstPluginUGen : MultiOutUGen {
	*ar { arg input, nout=2, gui=0, bypass=0;
		var nin = input.isArray.if {input.size} {input.notNil.asInt};
		^this.multiNewList([\audio, nout, gui, bypass, nin] ++ input);
	}
	init { arg nout ... theInputs;
		inputs = theInputs;
		^this.initOutputs(nout, rate);
	}
	checkInputs {
		inputs.do({ arg item, i;
			(i > 2).if {(item.rate != \audio).if {
                ^"input % must be audio rate".format(i).throw;
			}};
		});
        ^this.checkValidInputs;
    }
}

VstPlugin : Synth {
	// public
	var <name;
	var <numInputs;
	var <numOutputs;
	var <singlePrecision;
	var <doublePrecision;
	var <midiInput;
	var <midiOutput;
	var <programs;
	var <currentProgram;
	var <parameters;
	// private
	var paramFunc;
	var paramNameFunc;
	var programNameFunc;
	var programFunc;

	*def { arg name, nin=2, nout=2, gui=false;
		^SynthDef.new(name, {arg in, out, bypass=0;
			var vst = VstPluginUGen.ar(In.ar(in, nin.max(1)), nout.max(1), gui.asInt, bypass);
			// "vst synth index: %".format(vst.isArray.if {vst[0].source.synthIndex} {vst.source.synthIndex}).postln;
			Out.ar(out, vst);
		});
	}
	*new { arg defName, args, target, addAction=\addToHead;
		^super.new(defName, args, target, addAction).init;
	}
	init {
		this.onFree({
			this.prFree();
		});
		paramFunc = OSCFunc({ arg msg;
			// "param %: %".format(msg[3].asInt, msg[4]).postln;
		}, '/vst_param', argTemplate: [nodeID, 2]);
		paramNameFunc = OSCFunc({ arg msg;
			var index, name;
			index = msg[3].asInt;
			name = VstPlugin.msg2string(msg, 1);
			parameters[index] = name;
		}, '/vst_param_name', argTemplate: [nodeID, 2]);
		programFunc = OSCFunc({ arg msg;
			currentProgram = msg[3].asInt;
		}, '/vst_program', argTemplate: [nodeID, 2]);
		programNameFunc = OSCFunc({ arg msg;
			var index, name;
			index = msg[3].asInt;
			name = VstPlugin.msg2string(msg, 1);
			programs[index] = name;
		}, '/vst_program_name', argTemplate: [nodeID, 2]);
	}
	free { arg sendFlag=true;
		this.prFree();
		^super.free(sendFlag);
	}
	prFree {
		"VstPlugin freed!".postln;
		paramFunc.free;
		paramNameFunc.free;
		programFunc.free;
		programFunc.free;
	}
	info {
		"---".postln;
		"name: %".format(name).postln;
		"num inputs: %, num outputs: %".format(numInputs, numOutputs).postln;
		"midi input: %, midi output: %".format(midiInput, midiOutput).postln;
		"parameters (%):".format(parameters.size).postln;
		parameters.do({ arg item, i;
			"[%] %".format(i, item).postln;
		});
		"programs (%):".format(programs.size).postln;
		programs.do({ arg item, i;
			"[%] %".format(i, item).postln;
		});
		// todo
	}
	open { arg path, onSuccess, onFail;
		// the UGen will respond to the '/open' message with the following messages:
		OSCFunc.new({arg msg;
			msg[3].asBoolean.if {onSuccess.value(this);} {this.prOnFail; onFail.value(this)};
		}, '/vst_open', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; name = VstPlugin.msg2string(msg)}, '/vst_name', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; numInputs = msg[3].asInt}, '/vst_nin', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; numOutputs = msg[3].asInt}, '/vst_nout', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; midiInput = msg[3].asBoolean}, '/vst_midiin', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; midiOutput = msg[3].asBoolean}, '/vst_midiout', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; programs = Array.fill(msg[3].asInt, nil)}, '/vst_nprograms', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; currentProgram = msg[3].asInt}, '/vst_program', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; parameters = Array.fill(msg[3].asInt, nil)}, '/vst_nparams', argTemplate: [nodeID, 2]).oneShot;

		this.sendMsg('/open', path);
	}
	prOnFail {
		"open plugin failed!".postln;
		name = nil; numInputs = nil; numOutputs = nil; midiInput = nil; midiOutput = nil; parameters = nil; programs = nil;
	}
	close {
		this.sendMsg('/close');
	}
	// parameters
	setParameter { arg index, value;
		this.sendMsg('/param_set', index, value);
	}
	mapParameter { arg index, bus;
		this.sendMsg('/param_map', index, bus);
	}
	unmapParameter { arg index;
		this.mapParam(index, -1);
	}
	numParameters {
		^parameters.size;
	}
	// programs and banks
	numPrograms {
		^programs.size;
	}
	setProgram { arg index;
		index = index.clip(0, programs.size-1);
		this.sendMsg('/program_set', index);
		currentProgram = index;
	}
	setProgramName { arg name;
		this.sendMsg('/program_name', name);
	}
	readProgram { arg path;
		this.sendMsg('/program_read', path);
	}
	writeProgram { arg path;
		this.sendMsg('/program_write', path);
	}
	setProgramData { arg data;
		this.sendMsg('/program_data_set', data);
	}
	getProgramData { arg action;
		OSCFunc({ arg msg;
			var len, data;
			len = msg.size - 3; // skip address, nodeID and index
			data = Int8Array.new(len);
			len.do({ arg i;
				data.add(msg[i + 3]);
			});
			action.value(data);
		}, '/vst_program_data', argTemplate: [nodeID]).oneShot;
		this.sendMsg('/program_data_get');
	}
	readBank { arg path;
		this.sendMsg('/bank_read', path);
	}
	writeBank { arg path;
		this.sendMsg('/bank_write', path);
	}
	setBankData { arg data;
		this.sendMsg('/bank_data_set', data);
	}
	getBankData { arg action;
		OSCFunc({ arg msg;
			var len, data;
			len = msg.size - 3; // skip address, nodeID and index
			data = Int8Array.new(len);
			len.do({ arg i;
				data.add(msg[i + 3]);
			});
			action.value(data);
		}, '/vst_bank_data', argTemplate: [nodeID, 2]).oneShot;
		this.sendMsg('/bank_data_get');
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
		this.sendMsg('/midi_msg', status, data1, data2);
	}
	midiSysex { arg msg;
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
	getTransportPos { arg action;
		OSCFunc({ arg msg;
			action.value(msg[3]);
		}, '/vst_transport', argTemplate: [nodeID, 2]).oneShot;
		this.sendMsg('/transport_get');
	}
	// internal
	sendMsg { arg cmd ... args;
		server.sendBundle(0, ['/u_cmd', nodeID, 2, cmd] ++ args);
	}
	prMidiMsg { arg type, chn, data1=0, data2=0;
		var status = (chn - 1).clip(0, 15) + type;
		this.midiMsg(status, data1, data2);
	}
	*msg2string { arg msg, onset=0;
		var len, string;
		onset = onset + 3; // skip address, nodeID and synthIndex
		len = msg.size - onset;
		string = String.new(len);
		len.do({ arg i;
			string.add(msg[i + onset].asInt.asAscii);
		});
		^string;
	}
}

