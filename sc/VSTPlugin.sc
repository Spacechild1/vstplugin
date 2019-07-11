VSTPlugin : MultiOutUGen {
	// class members
	classvar counter = 0;
	classvar pluginDict;
	classvar parentInfo;
	classvar <platformExtension;
	// instance members
	var <id;
	// class methods
	*initClass {
		StartUp.add {
			platformExtension = Platform.case(
				\windows, ".dll", \osx, ".vst", \linux, ".so"
			);
			pluginDict = IdentityDictionary.new;
			parentInfo = (
				print: #{ arg self, long = false;
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
				toString: #{ arg self, sep = $\n;
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
				printParameters: #{ arg self;
					self.numParameters.do { arg i;
						var label;
						label = (self.parameterLabels[i].size > 0).if { "(%)".format(self.parameterLabels[i]) };
						"[%] % %".format(i, self.parameterNames[i], label ?? "").postln;
					};
				},
				printPrograms: #{ arg self;
					self.programNames.do { arg item, i;
						"[%] %".format(i, item).postln;
					};
				}
			);
		}
	}
	*ar { arg input, numOut=1, bypass=0, params, id, info;
		var numIn = 0;
		input.notNil.if {
			numIn = input.isArray.if {input.size} { 1 };
		}
		^this.multiNewList([\audio, id, info, numOut, bypass, numIn] ++ input ++ params);
	}
	*kr { ^this.shouldNotImplement(thisMethod) }

	*plugins { arg server;
		server = server ?? Server.default;
		^pluginDict[server];
	}
	*pluginList { arg server, sorted = false;
		var dict = this.plugins(server);
		var array = [];
		dict.notNil.if {
			// get list of unique plugins (which have been inserted under various keys)
			array = dict.as(IdentitySet).asArray;
			sorted.if {
				// sort by name
				array = array.sort({ arg a, b; a.name.compare(b.name, true) < 0});
			}
		};
		^array;
	}
	*pluginKeys { arg server;
		^this.pluginList(server).collect({ arg i; i.key });
	}
	*print { arg server;
		// print plugins sorted by name in ascending order
		// (together with path to distinguish plugins of the same name)
		this.pluginList(server, true).do { arg item;
			"% (%) [%]".format(item.key, item.vendor, item.path).postln; // rather print key instead of name
		};
	}
	*reset { arg server;
		server = server ?? Server.default;
		// clear plugin dictionary
		pluginDict[server] = IdentityDictionary.new;
	}
	*search { arg server, dir, useDefault=true, verbose=false, wait = -1, action;
		server = server ?? Server.default;
		// add dictionary if it doesn't exist yet
		pluginDict[server].isNil.if { pluginDict[server] = IdentityDictionary.new };
		server.isLocal.if { this.prSearchLocal(server, dir, useDefault, verbose, action) }
		{ this.prSearchRemote(server, dir, useDefault, verbose, wait, action) };
	}
	*searchMsg { arg dir, useDefault=true, verbose=false, dest=nil;
		var flags;
		dir.isString.if { dir = [dir] };
		(dir.isNil or: dir.isArray).not.if { ^"bad type for 'dir' argument!".throw };
		dir = dir.collect { arg p; this.prResolvePath(p) };
		// use remote search (won't write to temp file)!
		flags = (useDefault.asInteger) | (verbose.asInteger << 1);
		dest = this.prMakeDest(dest);
		^['/cmd', '/vst_search', flags, dest] ++ dir;
	}
	*prSearchLocal { arg server, dir, useDefault, verbose, action;
		{
			var dict = pluginDict[server];
			var filePath = PathName.tmp ++ this.prUniqueID.asString;
			// ask VSTPlugin to store the search results in a temp file
			server.listSendMsg(this.searchMsg(dir, useDefault, verbose, filePath));
			// wait for cmd to finish
			server.sync;
			// read file
			protect {
				File.use(filePath, "rb", { arg file;
					// plugins are seperated by newlines
					file.readAllString.split($\n).do { arg line;
						var info;
						(line.size > 0).if {
							info = this.prParseInfo(line);
							// store under key
							dict[info.key] = info;
						}
					};
				});
			} { arg error;
				error.notNil.if { "Failed to read tmp file!".warn };
				// done - free temp file
				File.delete(filePath).not.if { ("Could not delete data file:" + filePath).warn };
				action.value;
			};
		}.forkIfNeeded;
	}
	*prSearchRemote { arg server, dir, useDefault, verbose, wait, action;
		{
			var dict = pluginDict[server];
			var buf = Buffer(server); // get free Buffer
			// ask VSTPlugin to store the search results in this Buffer
			// (it will allocate the memory for us!)
			server.listSendMsg(this.searchMsg(dir, useDefault, verbose, buf));
			// wait for cmd to finish and update buffer info
			server.sync;
			buf.updateInfo({
				// now read data from Buffer
				buf.getToFloatArray(wait: wait, timeout: 5, action: { arg array;
					var string = array.collectAs({arg c; c.asInteger.asAscii}, String);
					string.split($\n).do { arg line;
						var info;
						(line.size > 0).if {
							info = this.prParseInfo(line);
							// store under key
							dict[info.key] = info;
						}
					};
					buf.free;
					action.value; // done
				});
			});
		}.forkIfNeeded;
	}
	*prParseInfo { arg string;
		var nparam, nprogram;
		// data is seperated by tabs
		var data = string.split($\t);
		var info = this.prMakeInfo(data);
		// get parameters (name + label)
		nparam = info.numParameters;
		info.parameterNames = Array.new(nparam);
		info.parameterLabels = Array.new(nparam);
		nparam.do { arg i;
			var onset = 12 + (i * 2);
			info.parameterNames.add(data[onset]);
			info.parameterLabels.add(data[onset + 1]);
		};
		// create inverse parameter mapping (name -> index)
		info.parameterIndex = IdentityDictionary.new;
		info.parameterNames.do { arg name, idx;
			info.parameterIndex[name.asSymbol] = idx;
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
		server = server ?? Server.default;
		// resolve the path
		path = this.prResolvePath(path);
		// add dictionary if it doesn't exist yet
		pluginDict[server].isNil.if { pluginDict[server] = IdentityDictionary.new };
		server.isLocal.if { this.prProbeLocal(server, path, key, action); }
		{ this.prProbeRemote(server, path, key, wait, action); };
	}
	*probeMsg { arg path, dest=nil;
		path = this.prResolvePath(path);
		dest = this.prMakeDest(dest);
		// use remote probe (won't write to temp file)!
		^['/cmd', '/vst_probe', path, dest];
	}
	*prProbeLocal { arg server, path, key, action;
		{
			var info, dict = pluginDict[server];
			var tmpPath = PathName.tmp ++ this.prUniqueID.asString;
			// ask server to write plugin info to tmp file
			server.listSendMsg(this.probeMsg(path, tmpPath));
			// wait for cmd to finish
			server.sync;
			// read file (only written if the plugin could be probed)
			File.exists(tmpPath).if {
				protect {
					File.use(tmpPath, "rb", { arg file;
						info = this.prParseInfo(file.readAllString);
						// store under key
						dict[info.key] = info;
						// also store under resolved path and custom key
						dict[path.asSymbol] = info;
						key !? { dict[key.asSymbol] = info };
					});
				} { arg error;
					error !? { "Failed to read tmp file!".warn };
					File.delete(tmpPath).not.if { ("Could not delete temp file:" + tmpPath).warn };
				}
			};
			// done (on fail, 'info' is nil)
			action.value(info);
		}.forkIfNeeded;
	}
	*prProbeRemote { arg server, path, key, wait, action;
		{
			var dict = pluginDict[server];
			var buf = Buffer(server); // get free Buffer
			// ask VSTPlugin to store the probe result in this Buffer
			// (it will allocate the memory for us!)
			server.listSendMsg(this.probeMsg(path, buf));
			// wait for cmd to finish and update buffer info
			server.sync;
			buf.updateInfo({
				// now read data from Buffer
				buf.getToFloatArray(wait: wait, timeout: 5, action: { arg array;
					var string = array.collectAs({arg c; c.asInteger.asAscii}, String);
					var info;
					(string.size > 0).if {
						info = this.prParseInfo(string);
						// store under key
						dict[info.key] = info;
						// also store under resolved path and custom key
						dict[path.asSymbol] = info;
						key !? { dict[key.asSymbol] = info };
					};
					buf.free;
					action.value(info); // done
				});
			});
		}.forkIfNeeded;
	}
	*prMakeInfo { arg data;
		var f, flags;
		f = data[11].asInteger;
		flags = Array.fill(8, {arg i; ((f >> i) & 1).asBoolean });
		// we have to use an IdentityDictionary instead of Event because the latter
		// overrides the 'asUGenInput' method, which we implicitly need in VSTPlugin.ar
		^IdentityDictionary.new(parent: parentInfo, know: true).putPairs([
			key: data[0].asSymbol,
			path: data[1].asString,
			name: data[2].asString,
			vendor: data[3].asString,
			category: data[4].asString,
			version: data[5].asString,
			id: data[6].asInteger,
			numInputs: data[7].asInteger,
			numOutputs: data[8].asInteger,
			numParameters: data[9].asInteger,
			numPrograms: data[10].asInteger,
			hasEditor: flags[0],
			isSynth: flags[1],
			singlePrecision: flags[2],
			doublePrecision: flags[3],
			midiInput: flags[4],
			midiOutput: flags[5],
			sysexInput: flags[6],
			sysexOutput: flags[7]
		]);
	}
	*prGetInfo { arg server, key, wait, action;
		var info, dict = pluginDict[server];
		key = key.asSymbol;
		// if the key already exists, return the info.
		// otherwise probe the plugin and store it under the given key
		dict !? { info = dict[key] } !? { action.value(info) }
		?? { this.probe(server, key, key, wait, action) };
	}
	*prResolvePath { arg path;
		var root, temp;
		path = path.asString;
		(thisProcess.platform.name == \windows).if {
			// replace / with \ because of a bug in PathName
			path = path.tr($/, $\\);
		};
		path = path.standardizePath; // expand ~/
		// other methods don't work for folders...
		PathName(path).isAbsolutePath.not.if {
			// resolve relative paths to the currently executing file
			root = thisProcess.nowExecutingPath;
			root.notNil.if {
				temp = root.dirname +/+ path;
				// first check if it's an existing folder
				PathName(temp).isFolder.if { ^temp };
				// otherwise treat it as a file path
				// no extension: append VST2 platform extension
				(path.find(".vst3").isNil && path.find(platformExtension).isNil).if {
					temp = temp ++ platformExtension;
				};
				// check if the file actually exists
				PathName(temp).isFile.if { ^temp };
			}
			// otherwise the path is passed to the UGen which tries
			// to resolve it to the standard VST search paths.
		};
		^path;
	}
	*prUniqueID {
		var id = counter;
		counter = counter + 1;
		^id;
	}
	*prMakeDest { arg dest;
		// 'dest' may be a) temp file path, b) bufnum, c) Buffer, d) nil
		dest !? {
			dest.isString.if {
				^dest; // tmp file path
			};
			dest = dest.asUGenInput;
			dest.isNumber.if { ^dest.asInteger }; // bufnum
			^"bad type for 'dest' argument (%)!".throw;
		}
		^-1; // invalid bufnum: don't write results
	}

	// instance methods
	init { arg theID, info, numOut, bypass, numInputs ... theInputs;
		var inputArray, paramArray;
		id = theID; // store ID
		inputArray = theInputs[..(numInputs-1)];
		paramArray = theInputs[numInputs..];
		// substitute parameter names with indices
		paramArray.pairsDo { arg param, value, i;
			param.isNumber.not.if {
				info ?? { ^Error("can't resolve parameter '%' without info".format(param)).throw; };
				param = info.parameterIndex[param.asSymbol];
				param ?? { ^Error("bad parameter '%' for plugin '%'".format(param, info.name)).throw; };
				paramArray[i] = param;
			};
		};
		inputs = [bypass, numInputs] ++ inputArray ++ paramArray;
		^this.initOutputs(numOut, rate)
	}
}
