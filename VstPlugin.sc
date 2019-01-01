VstPluginUGen : MultiOutUGen {
	*ar { arg input, nout=2, gui=0, bypass=0;
		var nin = input.isArray.if {input.size} {input.notNil.asInt};
		^this.multiNewList([\audio, nout, gui, bypass, nin] ++ input);
	}
	init { arg nout ... theInputs;
		inputs = theInputs;
		inputs.postln;
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
	var synthIndex_;

	*def { arg name, nin=2, nout=2, gui=false;
		^SynthDef.new(name, {arg in, out, bypass=0;
			var vst = VstPluginUGen.ar(In.ar(in, nin.max(1)), nout.max(1), gui.asInt, bypass);
			"vst synth index: %".format(vst.isArray.if {vst[0].source.synthIndex} {vst.source.synthIndex}).postln;
			Out.ar(out, vst);
		});
	}
	*new { arg defName, args, target, addAction=\addToHead;
		^super.new(defName, args, target, addAction).init;
	}
	init {
		^this;
	}
	info {
		this.sendMsg('/info');
	}
	open { arg path;
		this.sendMsg('/open', path);
	}
	close {
		this.sendMsg('/close');
	}
	setParam { arg index, value;
		this.sendMsg('/param_set', index, value);
	}
	mapParam { arg index, bus;
		this.sendMsg('/param_map', index, bus);
	}
	unmapParam { arg index;
		this.mapParam(index, -1);
	}

	sendMsg { arg cmd ... args;
		server.sendBundle(0, ['/u_cmd', nodeID, 2, cmd] ++ args);
	}
}

