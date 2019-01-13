VstPluginUGen : MultiOutUGen {
	*ar { arg input, nout=2, bypass=0;
		var nin = input.isArray.if {input.size} {input.notNil.asInt};
		^this.multiNewList([\audio, nout, bypass, nin] ++ input);
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
	classvar counter = 0;
	// public
	var <loaded;
	var <name;
	var <hasEditor;
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
	// private
	var oscFuncs;
	var useParamDisplay;
	var scGui;

	*makeSynthDef { arg name, nin=2, nout=2, replace=false;
		name = name ?? {"vst__%".format(counter = counter + 1)};
		^SynthDef.new(name, {arg in, out, bypass=0;
			var vst = VstPluginUGen.ar(In.ar(in, nin.max(1)), nout.max(1), bypass);
			replace.if { ReplaceOut.ar(out, vst) } { Out.ar(out, vst) };
		});
	}
	*new { arg defName, args, target, addAction=\addToHead;
		^super.new(defName, args, target, addAction).init;
	}
	*newPaused { arg defName, args, target, addAction=\addToHead;
		^super.newPaused(defName, args, target, addAction).init;
	}
	*replace { arg nodeToReplace, defName, args, sameID=false;
		^super.replace(nodeToReplace, defName, args, sameID).init;
	}
	init {
		this.onFree({
			this.prFree();
		});
		loaded = false;
		oscFuncs = List.new;
		oscFuncs.add(OSCFunc({ arg msg;
			var index, value;
			index = msg[3].asInt;
			value = msg[4].asFloat;
			// we have to defer the method call to the AppClock!
			{scGui.notNil.if { scGui.paramValue(index, value) }}.defer;
		}, '/vst_param', argTemplate: [nodeID, 2]));
		oscFuncs.add(OSCFunc({ arg msg;
			var index, string;
			index = msg[3].asInt;
			string = VstPlugin.msg2string(msg, 1);
			// we have to defer the method call to the AppClock!
			{scGui.notNil.if { scGui.paramDisplay(index, string) }}.defer;
		}, '/vst_param_display', argTemplate: [nodeID, 2]));
		oscFuncs.add(OSCFunc({ arg msg;
			var index, name;
			index = msg[3].asInt;
			name = VstPlugin.msg2string(msg, 1);
			parameterNames[index] = name;
		}, '/vst_param_name', argTemplate: [nodeID, 2]));
		oscFuncs.add(OSCFunc({ arg msg;
			var index, name;
			index = msg[3].asInt;
			name = VstPlugin.msg2string(msg, 1);
			parameterLabels[index] = name;
		}, '/vst_param_label', argTemplate: [nodeID, 2]));
		oscFuncs.add(OSCFunc({ arg msg;
			currentProgram = msg[3].asInt;
		}, '/vst_program', argTemplate: [nodeID, 2]));
		oscFuncs.add(OSCFunc({ arg msg;
			var index, name;
			index = msg[3].asInt;
			name = VstPlugin.msg2string(msg, 1);
			programs[index] = name;
		}, '/vst_program_name', argTemplate: [nodeID, 2]));
	}
	free { arg sendFlag=true;
		this.prFree();
		^super.free(sendFlag);
	}
	prFree {
		"VstPlugin freed!".postln;
		oscFuncs.do({ arg func;
			func.free;
		});
		this.prClear();
		this.prClearGui();
	}
	showGui { arg show=true;
		loaded.if {
			scGui.isNil.if { this.sendMsg('/vis', show.asInt)} { scGui.view.visible_(show)};
		}
	}
	info {
		loaded.if {
			"---".postln;
			"name: %".format(name).postln;
			"editor: %".format(hasEditor).postln;
			"num inputs: %, num outputs: %".format(numInputs, numOutputs).postln;
			"midi input: %, midi output: %".format(midiInput, midiOutput).postln;
			"parameters (%):".format(this.numParameters).postln;
			this.numParameters.do({ arg i;
				var label;
				label = (parameterLabels[i].size > 0).if { "(%)".format(parameterLabels[i]) };
				"[%] % %".format(i, parameterNames[i], label ?? "").postln;
			});
			"programs (%):".format(programs.size).postln;
			programs.do({ arg item, i;
				"[%] %".format(i, item).postln;
			});
		} {
			"---".postln;
			"no plugin loaded!".postln;
		}
	}
	open { arg path, onSuccess, onFail, gui=\sc, paramDisplay=true, info=false;
		var flags;
		this.prClearGui();
		// the UGen will respond to the '/open' message with the following messages:
		OSCFunc.new({arg msg; name = VstPlugin.msg2string(msg)}, '/vst_name', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; hasEditor = msg[3].asBoolean}, '/vst_editor', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; numInputs = msg[3].asInt}, '/vst_nin', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; numOutputs = msg[3].asInt}, '/vst_nout', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; midiInput = msg[3].asBoolean}, '/vst_midiin', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; midiOutput = msg[3].asBoolean}, '/vst_midiout', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg; programs = Array.fill(msg[3].asInt, nil)}, '/vst_nprograms', argTemplate: [nodeID, 2]).oneShot;
		OSCFunc.new({arg msg;
			var n = msg[3].asInt; parameterNames = Array.fill(n, nil); parameterLabels = Array.fill(n, nil);
		}, '/vst_nparams', argTemplate: [nodeID, 2]).oneShot;
		// the /open message is sent after all the other messages above and after parameter names and program names
		// but *before* parameter values are sent (so the GUI has a chance to respond to it)
		OSCFunc.new({arg msg;
			msg[3].asBoolean.if {
				loaded = true;
				// make SC editor if needed/wanted (deferred to AppClock!)
				(gui != \none && (gui == \sc || hasEditor.not)).if { {scGui = VstPluginGui.new(this, paramDisplay)}.defer };
				// print info if wanted
				info.if { this.info };
				onSuccess.value(this);
			} {
				loaded = false;
				this.prClear();
				onFail.value(this);
			};
		}, '/vst_open', argTemplate: [nodeID, 2]).oneShot;
		flags = (gui == \vst).asInt | (paramDisplay.asBoolean.asInt << 1);
		this.sendMsg('/open', flags, path);
	}
	prClear {
		loaded = false; name = nil; numInputs = nil; numOutputs = nil; midiInput = nil; midiOutput = nil;
		parameterNames = nil; parameterLabels = nil; programs = nil; currentProgram = nil; hasEditor = nil;
	}
	prClearGui {
		scGui.notNil.if { {scGui.view.remove}.defer }; scGui = nil;
	}
	close {
		this.sendMsg('/close');
		this.prClear();
		this.prClearGui();
	}
	// parameters
	setParameter { arg index, value;
		((index >= 0) && (index < this.numParameters)).if {
			this.sendMsg('/param_set', index, value);
			scGui.notNil.if { scGui.paramValue(index, value) };
		} {
			^"parameter index % out of range".format(index).throw;
		};
	}
	mapParameter { arg index, bus;
		((index >= 0) && (index < this.numParameters)).if {
			this.sendMsg('/param_map', index, bus);
		} {
			^"parameter index % out of range".format(index).throw;
		};
	}
	unmapParameter { arg index;
		this.mapParam(index, -1);
	}
	numParameters {
		^parameterNames.size;
	}
	// programs and banks
	numPrograms {
		^programs.size;
	}
	setProgram { arg index;
		((index >= 0) && (index < this.numPrograms)).if {
			this.sendMsg('/program_set', index);
			currentProgram = index;
		} {
			^"program number % out of range".format(index).throw;
		};
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
		this.sendMsg('/midi_msg', Int8Array.with(status, data1, data2));
	}
	midiSysex { arg msg;
		(msg.class != Int8Array).if {^"'%' expects Int8Array!".format(thisMethod.name).throw;};
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
	prMidiMsg { arg hiStatus, lowStatus, data1=0, data2=0;
		var status = hiStatus.asInt + lowStatus.asInt.clip(0, 15);
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

VstPluginGui {
	classvar <>maxParams = 12; // max. number of parameters per column
	classvar <>sliderWidth = 180;
	classvar <>sliderHeight = 20;
	// public
	var <view;
	// private
	var paramSliders;
	var paramDisplays;
	var bDisplay;
	var model;

	*new { arg model, paramDisplay;
		^super.new.init(model, paramDisplay);
	}

	init { arg theModel, paramDisplay;
		var nparams, title, ncolumns, nrows, bounds, layout, font;

		model = theModel;
		bDisplay = paramDisplay.asBoolean;

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
			name = StaticText.new().string_("%: %".format(i, model.parameterNames[i]));
			label = StaticText.new().string_(model.parameterLabels[i] ?? "");
			display = StaticText.new();
			paramDisplays.add(display);
			slider = Slider.new(bounds: sliderWidth@sliderHeight).fixedHeight_(sliderHeight);
			slider.action = {arg s; model.setParameter(i, s.value)};
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

	paramValue { arg index, value;
		paramSliders.at(index).value = value;
	}

	paramDisplay { arg index, string;
		paramDisplays.at(index).string = string;
	}
}

