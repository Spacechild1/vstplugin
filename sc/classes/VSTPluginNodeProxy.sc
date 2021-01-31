VSTSynthControl : SynthControl {
	var <synth;

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

	playToBundle { arg bundle, extraArgs, target, addAction = 1;
		var nodeID = super.playToBundle(bundle, extraArgs, target, addAction);
		synth = Synth.basicNew(this.asDefName, target.server, nodeID);
		NodeWatcher.register(synth, false);
		^nodeID
	}
}

VSTPluginNodeProxyController : VSTPluginController {
	var <proxy, hasStarted = false;

	*initClass {
		// add NodeProxy roles
		Class.initClassTree(AbstractPlayControl);

		// \vst role (Function)
		AbstractPlayControl.proxyControlClasses.put(\vst, VSTSynthDefControl);
		AbstractPlayControl.buildMethods.put(\vst,
			#{ arg func, proxy, channelOffset = 0, index;
				func.buildForProxy(proxy, channelOffset, index);
			}
		);

		// \vstFilter role
		AbstractPlayControl.proxyControlClasses.put(\vstFilter, VSTSynthDefControl);
		AbstractPlayControl.buildMethods.put(\vstFilter, AbstractPlayControl.buildMethods[\filter]);

		// \vstDef role (SynthDef)
		AbstractPlayControl.proxyControlClasses.put(\vstDef, VSTSynthControl);
		AbstractPlayControl.buildMethods.put(\vstDef,
			#{ arg def, proxy, channelOffset = 0, index;
				def.buildForProxy(proxy, channelOffset, index);
			}
		);
	}

	*new { arg proxy, index = 0, id, wait = -1;
		var control = proxy.objects[index];
		// For VSTPluginSynthControl, the SynthDef is 'nil' and must be inferred
		var def = control.tryPerform(\synthDef);
		var synth = control.tryPerform(\synth) ?? { MethodError("Unsupported NodeProxy source", this).throw;
		};
		^super.new(synth, id, def, wait)
		    .initProxyAndCondition(proxy)
		    .checkIfStarted(synth);
	}

	*collect { arg proxy, index = 0, ids, wait = -1;
		var control = proxy.objects[index];
		// For VSTPluginSynthControl, the SynthDef is 'nil' and must be inferred
		var def = control.tryPerform(\synthDef);
		var synth = control.tryPerform(\synth) ?? { MethodError("Unsupported NodeProxy source", this).throw;
		};
		var plugins = this.prFindPlugins(synth, def);
		^this.prCollect(plugins, ids, { arg desc;
			var info = desc.key !? { VSTPlugin.plugins(synth.server)[desc.key] };
			// 'newCopyArgs' avoids calling VSTPluginController.new!
			super.newCopyArgs.init(synth, desc.index, wait, info)
			    .initProxyAndCondition(proxy)
		        .checkIfStarted(synth);
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
