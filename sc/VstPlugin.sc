VstPlugin : MultiOutUGen {
	// class members
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
					self.toString.postln;
					long.if {
						"".postln;
						"parameters (%):".format(self.numParameters).postln;
						self.printParameters;
						"".postln;
						"programs (%):".format(self.numPrograms).postln;
						self.printPrograms;
					};
					"".postln;
				},
				toString: { arg self, sep = $\n;
					var s;
					s = "name: %".format(self.name) ++ sep
					++ "path: %".format(self.path) ++ sep
					++ "vendor: %".format(self.vendor) ++ sep
					++ "category: %".format(self.category) ++ sep
					++ "version: %".format(self.version) ++ sep
					++ "input channels: %".format(self.numInputs) ++ sep
					++ "output channels: %".format(self.numOutputs) ++ sep
					++ "parameters: %".format(self.numParameters) ++ sep
					++ "programs: %".format(self.numPrograms) ++ sep
					++ "MIDI input: %".format(self.midiInput) ++ sep
					++ "MIDI output: %".format(self.midiOutput) ++ sep
					++ "sysex input: %".format(self.sysexInput) ++ sep
					++ "sysex output: %".format(self.sysexOutput) ++ sep
					++ "synth: %".format(self.isSynth) ++ sep
					++ "editor: %".format(self.hasEditor) ++ sep
					++ "single precision: %".format(self.singlePrecision) ++ sep
					++ "double precision: %".format(self.doublePrecision);
				},
				printParameters: { arg self;
					self.numParameters.do { arg i;
						var label;
						label = (self.parameterLabels[i].size > 0).if { "(%)".format(self.parameterLabels[i]) };
						"[%] % %".format(i, self.parameterNames[i], label ?? "").postln;
					};
				},
				printPrograms: { arg self;
					self.programNames.do { arg item, i;
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

	*plugins { arg server;
		server = server ?? Server.default;
		^pluginDict[server];
	}
	*pluginKeys { arg server;
		var dict = this.plugins(server);
		dict.notNil.if {
			// return sorted list of plugin keys (case insensitive)
			^Array.newFrom(dict.keys).sort({ arg a, b; a.asString.compare(b.asString, true) < 0});
		} { ^[] };
	}
	*print { arg server;
		this.pluginKeys(server).do { arg p; p.postln; }
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
		path = path.collect { arg p; this.prResolvePath(p) };
		// add dictionary if it doesn't exist yet
		pluginDict[server].isNil.if { pluginDict[server] = IdentityDictionary.new };
		server.isLocal.if { this.prSearchLocal(server, path, useDefault, verbose, action) }
		{ this.prSearchRemote(server, path, useDefault, verbose, wait, action) };
	}
	*prSearchLocal { arg server, searchPaths, useDefault, verbose, action;
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
		info = this.prMakeInfo(key, data[1..11]);
		// get parameters (name + label)
		nparam = info.numParameters;
		info.parameterNames = Array.new(nparam);
		info.parameterLabels = Array.new(nparam);
		nparam.do { arg i;
			var onset = 12 + (i * 2);
			info.parameterNames.add(data[onset]);
			info.parameterLabels.add(data[onset + 1]);
		};
		// get programs
		nprogram = info.numPrograms;
		info.programNames = Array.new(nprogram);
		nprogram.do { arg i;
			var onset = 12 + (nparam * 2) + i;
			info.programNames.add(data[onset]);
		};
		^info;
	}
	*probe { arg server, path, key, wait = -1, action;
		var cb;
		server = server ?? Server.default;
		// if key is nil, use the plugin path as key
		key = key.notNil.if { key.asSymbol } { path.asSymbol };
		// resolve the path
		path = this.prResolvePath(path);
		// add dictionary if it doesn't exist yet
		pluginDict[server].isNil.if { pluginDict[server] = IdentityDictionary.new };
		cb = { arg info;
			info.notNil.if {
				"'%' successfully probed".format(key).postln;
			} {
				"couldn't probe '%'".format(key).postln;
			};
			action.value(info, path); // also pass the resolved path
		};
		server.isLocal.if { this.prProbeLocal(server, path, key, cb); }
		{ this.prProbeRemote(server, path, key, wait, cb); };
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
					info.key = key; // overwrite
					pluginDict[server][key] = info;
					this.prQueryPlugin(server, key, wait, { action.value(info) });
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
		info.programNames = Array.new(num);
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
					info.programNames.add(result[i + 2].asSymbol);
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
		f = info[10].asInt;
		flags = Array.fill(8, {arg i; ((f >> i) & 1).asBoolean });
		^(
			parent: parentInfo,
			key: key,
			path: info[0].asString,
			name: info[1].asString,
			vendor: info[2].asString,
			category: info[3].asString,
			version: info[4].asString,
			id: info[5].asInt,
			numInputs: info[6].asInt,
			numOutputs: info[7].asInt,
			numParameters: info[8].asInt,
			numPrograms: info[9].asInt,
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
		pluginDict[server] !? { arg dict; dict[key] } !? { arg info; action.value(info, key) }
		?? { this.probe(server, key, key, wait, action) };
	}
	*prResolvePath { arg path;
		path = path.asString;
		(thisProcess.platform.name == \windows).if {
			// replace / with \ because of a bug in PathName
			path = path.tr($/, $\\);
		};
		path = path.standardizePath; // expand ~/
		// other methods don't work for folders...
		PathName(path).isAbsolutePath.not.if {
			// resolve relative paths to the currently executing file
			var root = thisProcess.nowExecutingPath;
			root.notNil.if {
				path = root.dirname +/+ path;
			} {
				"couldn't resolve '%' - relative paths only work on saved files!".error;
				path = nil;
			};
		};
		^path;
	}
	// instance methods
	init { arg theID, numOut ... theInputs;
		id = theID;
		inputs = theInputs;
		^this.initOutputs(numOut, rate)
	}
}
