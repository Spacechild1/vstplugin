VSTSynthControl : SynthControl {
	var <synth;

	*initClass {
		// \vst role
		AbstractPlayControl.proxyControlClasses.put(\vstDef, VSTSynthControl);
		AbstractPlayControl.buildMethods.put(\vstDef,
			#{ arg func, proxy, channelOffset = 0, index;
				func.buildForProxy(proxy, channelOffset, index);
			}
		);
	}

	asDefName { ^source.value }

	playToBundle { arg bundle, extraArgs, target, addAction = 1;
		var nodeID = super.playToBundle(bundle, extraArgs, target, addAction);
		synth = Synth.basicNew(this.asDefName, target.server, nodeID);
		NodeWatcher.register(synth, false);
		^nodeID
	}
}

VSTSynthDefControl : SynthDefControl {
	var <synth;

	*initClass {
		// \vstFunc role
		AbstractPlayControl.proxyControlClasses.put(\vst, VSTSynthDefControl);
		AbstractPlayControl.buildMethods.put(\vst,
			#{ arg func, proxy, channelOffset = 0, index;
				func.buildForProxy(proxy, channelOffset, index);
			}
		);
		// \vstFilter role
		AbstractPlayControl.proxyControlClasses.put(\vstFilter, VSTSynthDefControl);
		AbstractPlayControl.buildMethods.put(\vstFilter, AbstractPlayControl.buildMethods[\filter]);
	}

	playToBundle { arg bundle, extraArgs, target, addAction = 1;
		var nodeID = super.playToBundle(bundle, extraArgs, target, addAction);
		synth = Synth.basicNew(this.asDefName, target.server, nodeID);
		NodeWatcher.register(synth, false);
		^nodeID
	}
}

VSTPluginNodeProxyController : VSTPluginController {
	var <proxy, hasStarted = false;

	*new { arg proxy, index = 0, id, wait = -1;
		var control, def;
		control = proxy.objects[index];
		// For VSTPluginSynthControl, the SynthDef is 'nil' and must be inferred
		def = control.tryPerform(\synthDef);
		^super.new(control.synth, id, def, wait)
		    .initProxyAndCondition(proxy)
		    .checkIfStarted(control.synth);
	}

	*collect { arg proxy, index = 0, ids, wait = -1;
		var control = proxy.objects[index];
		// For VSTPluginSynthControl, the SynthDef is 'nil' and must be inferred
		var def = control.tryPerform(\synthDef);
		var plugins = this.prFindPlugins(control.synth, def);
		^this.prCollect(plugins, ids, { arg desc;
			var info = desc.key !? { VSTPlugin.plugins(control.synth.server)[desc.key] };
			// 'newCopyArgs' avoids calling VSTPluginController.new!
			super.newCopyArgs.init(control.synth, desc.index, wait, info)
			    .initProxyAndCondition(proxy)
		        .checkIfStarted(control.synth);
		});
	}

	initProxyAndCondition { arg inProxy; proxy = inProxy; hasStarted = Condition(false); }

	checkIfStarted { arg synth;
		var goFunc;
		synth.isPlaying.if {
			hasStarted.test = true;
			hasStarted.signal;
		} {
			goFunc = {|node, msg|
				(msg == \n_go).if {
					hasStarted.test = true;
					hasStarted.signal;
					synth.removeDependant(goFunc);
				}
			};
			synth.addDependant(goFunc);
		}
	}

	sendMsg { arg cmd ... args;
		// wait for node if needed
		hasStarted.test.if {
			synth.server.sendMsg('/u_cmd', synth.nodeID, synthIndex, cmd, *args);
		} {
			forkIfNeeded {
				hasStarted.wait;
				synth.server.sendMsg('/u_cmd', synth.nodeID, synthIndex, cmd, *args);
			}
		}
	}
}
