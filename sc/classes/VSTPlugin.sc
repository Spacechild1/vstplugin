VSTPlugin : MultiOutUGen {
	// class members
	classvar pluginDict;
	classvar parentInfo;
	classvar <platformExtension;
	// instance members
	var <id;
	var <info;
	// class methods
	*initClass {
		StartUp.add {
			platformExtension = Platform.case(
				\windows, ".dll", \osx, ".vst", \linux, ".so"
			);
			pluginDict = IdentityDictionary.new;
			pluginDict[Server.default] = IdentityDictionary.new;
			parentInfo = (
				numParameters: #{ arg self; self.parameters.size },
				numPrograms: #{ arg self; self.programs.size },
				findParamIndex: #{ arg self, name; self.prParamIndexMap[name.asSymbol] },
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
					var s, auxin = (self.numAuxInputs > 0), auxout = (self.numAuxOutputs > 0);
					s = "name: %".format(self.name) ++ sep
					++ "path: %".format(self.path) ++ sep
					++ "vendor: %".format(self.vendor) ++ sep
					++ "category: %".format(self.category) ++ sep
					++ "version: %".format(self.version) ++ sep
					++ "input channels: %".format(self.numInputs) ++ sep
					++ (auxin.if { "aux input channels: %".format(self.numAuxInputs) ++ sep } {""})
					++ "output channels: %".format(self.numOutputs) ++ sep
					++ (auxout.if { "aux output channels: %".format(self.numAuxOutputs) ++ sep } {""})
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
					self.parameters.do { arg param, i;
						var label;
						label = (param.label.size > 0).if { "(%)".format(param.label) };
						"[%] % %".format(i, param.name, label ?? "").postln;
					};
				},
				printPrograms: #{ arg self;
					self.program.do { arg pgm, i;
						"[%] %".format(i, pgm.name).postln;
					};
				},
				// deprecated (removed) methods from v0.1
				parameterNames: #{ arg self; Error("parameterNames is deprecated, use parameters[index].name").throw; },
				parameterLabels: #{ arg self; Error("parameterLabels is deprecated, use parameters[index].label").throw; },
				programNames: #{ arg self; Error("programNames is deprecated, use programs[index].name").throw; }
			);
		}
	}
	*ar { arg input, numOut=1, bypass=0, params, id, info, auxInput, numAuxOut=0;
		var flags = 0; // not used (yet)
		input = input.asArray;
		auxInput = auxInput.asArray;
		params = params.asArray;
		params.size.odd.if {
			^Error("'params': expecting pairs of param index/name + value").throw;
		};
		^this.multiNewList([\audio, id, info, numOut, numAuxOut, flags, bypass, input.size]
			++ input ++ params.size.div(2) ++ params ++ auxInput.size ++ auxInput);
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
	*clear { arg server, remove=true;
		server = server ?? Server.default;
		// clear local plugin dictionary
		pluginDict[server] = IdentityDictionary.new;
		// clear server plugin dictionary
		// remove=true -> also delete temp file
		server.listSendMsg(this.clearMsg(remove));
	}
	*clearMsg { arg remove=true;
		^['/cmd', '/vst_clear', remove.asInteger];
	}
	*reset { arg server;
		this.deprecated(thisMethod, this.class.findMethod(\clear));
	}
	*search { arg server, dir, useDefault=true, verbose=true, wait = -1, action, save=true, parallel=true;
		server = server ?? Server.default;
		// add dictionary if it doesn't exist yet
		pluginDict[server].isNil.if { pluginDict[server] = IdentityDictionary.new };
		server.isLocal.if { this.prSearchLocal(server, dir, useDefault, verbose, save, parallel, action) }
		{ this.prSearchRemote(server, dir, useDefault, verbose, save, parallel, wait, action) };
	}
	*searchMsg { arg dir, useDefault=true, verbose=false, save=true, parallel=true, dest=nil;
		var flags = 0;
		dir.isString.if { dir = [dir] };
		(dir.isNil or: dir.isArray).not.if { ^"bad type for 'dir' argument!".throw };
		dir = dir.collect { arg p; this.prResolvePath(p) };
		// make flags
		[useDefault, verbose, save, parallel].do { arg value, bit;
			flags = flags | (value.asBoolean.asInteger << bit);
		};
		dest = this.prMakeDest(dest); // nil -> -1 = don't write results
		^['/cmd', '/vst_search', flags, dest] ++ dir;
	}
	*prSearchLocal { arg server, dir, useDefault, verbose, save, parallel, action;
		{
			var stream, dict = pluginDict[server];
			var tmpPath = this.prMakeTmpPath;
			// ask VSTPlugin to store the search results in a temp file
			server.listSendMsg(this.searchMsg(dir, useDefault, verbose, save, parallel, tmpPath));
			// wait for cmd to finish
			server.sync;
			// read file
			try {
				File.use(tmpPath, "rb", { arg file;
					stream = CollStream.new(file.readAllString);
				});
				// done - free temp file
				File.delete(tmpPath).not.if { ("Could not delete tmp file:" + tmpPath).warn };
			} { "Failed to read tmp file!".error };
			stream.notNil.if {
				this.prParseIni(stream).do { arg info;
					// store under key
					dict[info.key] = info;
				};
			};
			action.value;
		}.forkIfNeeded;
	}
	*prSearchRemote { arg server, dir, useDefault, verbose, save, parallel, wait, action;
		{
			var dict = pluginDict[server];
			var buf = Buffer(server); // get free Buffer
			// ask VSTPlugin to store the search results in this Buffer
			// (it will allocate the memory for us!)
			server.listSendMsg(this.searchMsg(dir, useDefault, verbose, save, parallel, buf));
			// wait for cmd to finish and update buffer info
			server.sync;
			buf.updateInfo({
				// now read data from Buffer
				buf.getToFloatArray(wait: wait, timeout: 5, action: { arg array;
					var string = array.collectAs({arg c; c.asInteger.asAscii}, String);
					this.prParseIni(CollStream.new(string)).do { arg info;
						// store under key
						dict[info.key] = info;
					};
					buf.free;
					action.value; // done
				});
			});
		}.forkIfNeeded;
	}
	*stopSearch { arg server;
		server = server ?? Server.default;
		server.listSendMsg(this.stopSearchMsg);
	}
	*stopSearchMsg { ^['/cmd', '/vst_search_stop']; }
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
			var stream, info, dict = pluginDict[server];
			var tmpPath = this.prMakeTmpPath;
			// ask server to write plugin info to tmp file
			server.listSendMsg(this.probeMsg(path, tmpPath));
			// wait for cmd to finish
			server.sync;
			// read file (only written if the plugin could be probed)
			File.exists(tmpPath).if {
				try {
					File.use(tmpPath, "rb", { arg file;
						stream = CollStream.new(file.readAllString);
					});
				} { "Failed to read tmp file!".error };
				File.delete(tmpPath).not.if { ("Could not delete tmp file:" + tmpPath).warn };
				stream.notNil.if {
					info = this.prParseInfo(stream);
					// store under key
					dict[info.key] = info;
					// also store under resolved path and custom key
					dict[path.asSymbol] = info;
					key !? { dict[key.asSymbol] = info };
				};
			};
			// done (on fail, info is nil)
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
						info = this.prParseInfo(CollStream.new(string));
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
	*readPlugins {
		var path, stream, dict = IdentityDictionary.new;
		// handle 32-bit SuperCollider on Windows (should we care about 32-bit builds on macOS and Linux?)
		path = ((thisProcess.platform.name == \windows) && Platform.resourceDir.find("(x86").notNil).if
		{ "cache32.ini" } { "cache.ini" };
		path = ("~/.VSTPlugin/" ++ path).standardizePath;
		// read plugins.ini file
		File.exists(path).not.if {
			"Couldn't find plugin cache file! Make sure to call VSTPlugin.search at least once.".warn;
			^dict;
		};
		try {
			File.use(path, "rb", { arg file;
				stream = CollStream.new(file.readAllString);
			});
		} {
			"Failed to read plugin info file (%)!".format(path).error;
			^dict;
		};
		stream.notNil.if {
			this.prParseIni(stream).do { arg info;
				// store under key
				dict[info.key] = info;
			};
		};
		^dict;
	}
	*prGetLine { arg stream, skip=false;
		var pos, line;
		{
			pos = stream.pos;
			line = stream.readUpTo($\n);
			stream.pos_(pos + line.size + 1);
			line ?? { ^nil }; // EOF
			// skip empty lines or comments if desired
			(skip && ((line.size == 0) || (line[0] == $#) || (line[0] == $;))).if { ^nil };
			^line;
		}.loop;
	}
	*prParseCount { arg line;
		var onset = line.find("=");
		onset ?? { Error("plugin info: bad data (expecting 'n=<number>')").throw; };
		^line[(onset+1)..].asInteger; // will eat whitespace and stop at newline
	}
	*prTrim { arg str;
		var start, end;
		var isWhiteSpace = { arg c; (c == $ ) || (c == $\t) };
		(str.size == 0).if { ^str };
		start = block { arg break;
			str.do { arg c, i;
				isWhiteSpace.(c).not.if {
					break.value(i);
				}
			}
			^""; // all white space
		};
		end = block { arg break;
			str.reverseDo { arg c, i;
				isWhiteSpace.(c).not.if {
					break.value(str.size - i - 1);
				}
			}
		};
		^str[start..end]; // start and end can be both nil
	}
	*prParseKeyValuePair { arg line;
		var key, value, split = line.find("=");
		split ?? { Error("plugin info: bad data (expecting 'key=value')").throw; };
		key = line[0..(split-1)];
		split = split + 1;
		(split < line.size).if {
			value = line[split..];
		} { value = "" };
		^[this.prTrim(key).asSymbol, this.prTrim(value)];
	}
	*prParseIni { arg stream;
		var results, onset, line, n, indices, last = 0;
		// skip header
		line = this.prGetLine(stream, true);
		(line != "[plugins]").if { ^Error("missing [plugins] header").throw };
		// get number of plugins
		line = this.prGetLine(stream, true);
		n = this.prParseCount(line);
		results = Array.newClear(n);
		// now serialize plugins
		n.do { arg i;
			results[i] = this.prParseInfo(stream);
		};
		^results;
	}
	*prParseInfo { arg stream;
		var info = IdentityDictionary.new(parent: parentInfo, know: true);
		var parameters, indexMap, programs, keys;
		var line, key, value, onset, n, f, flags, plugin = false;
		var hex2int = #{ arg str;
			str.toUpper.ascii.reverse.sum { arg c, i;
				(c >= 65).if {
					(c - 55) << (i * 4);
				} {
					(c - 48) << (i * 4);
				}
			}
		};
		// default values:
		info.numAuxInputs = 0;
		info.numAuxOutputs = 0;
		{
			line = this.prGetLine(stream, true);
			line ?? { ^Error("EOF reached").throw };
			// line.postln;
			switch(line,
				"[plugin]", { plugin = true; },
				"[parameters]",
				{
					line = this.prGetLine(stream);
					n = this.prParseCount(line);
					parameters = Array.newClear(n);
					indexMap = IdentityDictionary.new;
					n.do { arg i;
						var name, label;
						line = this.prGetLine(stream);
						#name, label = line.split($,);
						parameters[i] = (
							name: this.prTrim(name),
							label: this.prTrim(label)
							// more info later
						);
					};
					info.parameters = parameters;
					parameters.do { arg param, index;
						indexMap[param.name.asSymbol] = index;
					};
					info.prParamIndexMap = indexMap;
				},
				"[programs]",
				{
					line = this.prGetLine(stream);
					n = this.prParseCount(line);
					programs = Array.newClear(n);
					n.do { arg i;
						var name = this.prGetLine(stream);
						programs[i] = (name: name); // more info later
					};
					info.programs = programs;
				},
				"[keys]",
				{
					line = this.prGetLine(stream);
					n = this.prParseCount(line);
					keys = Array.newClear(n);
					n.do { arg i;
						keys[i] = this.prGetLine(stream);
					};
					// take the first (primary) key
					info.key = keys[0].asSymbol;
					// *** EXIT POINT ***
					^info;
				},
				{
					// plugin
					plugin.not.if {
						^Error("plugin info: bad data (%)".format(line)).throw;
					};
					#key, value = this.prParseKeyValuePair(line);
					switch(key,
						\path, { info[key] = value },
						\name, { info[key] = value; },
						\vendor, { info[key] = value },
						\category, { info[key] = value },
						\version, { info[key] = value },
						\sdkVersion, { info[key] = value },
						\id, { info[key] = value },
						\inputs, { info.numInputs = value.asInteger },
						\outputs, { info.numOutputs = value.asInteger },
						\auxinputs, { info.numAuxInputs = value.asInteger },
						\auxoutputs, { info.numAuxOutputs = value.asInteger },
						\flags,
						{
							f = hex2int.(value);
							flags = Array.fill(8, {arg i; ((f >> i) & 1).asBoolean });
							info.putPairs([
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
					);
				},
			);
		}.loop;
	}
	*prGetInfo { arg server, key, wait, action;
		var info, dict = pluginDict[server];
		key = key.asSymbol;
		// if the key already exists, return the info.
		// otherwise probe the plugin and store it under the given key
		dict !? { info = dict[key] } !? { action.value(info) }
		?? { this.probe(server, key, key, wait, action) };
	}
	*prResolvePath { arg path, vst=true, exists=true;
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
				(vst && path.find(".vst3").isNil && path.find(platformExtension).isNil).if {
					temp = temp ++ platformExtension;
				};
				// check if the file actually exists
				(exists.not || PathName(temp).isFile).if { ^temp };
			}
			// otherwise the path is passed to the UGen which tries
			// to resolve it to the standard VST search paths.
		};
		^path;
	}
	*prMakeTmpPath {
		^PathName.tmp +/+ "vst_" ++ UniqueID.next;
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
	init { arg theID, theInfo, numOut, numAuxOut, flags, bypass ... args;
		var numInputs, inputArray, numParams, paramArray, numAuxInputs, auxInputArray, sym, offset=0;
		// store id and info (both optional)
		id = theID;
		info = theInfo;
		// main inputs
		numInputs = args[offset];
		(numInputs > 0).if {
			inputArray = args[(offset+1)..(offset+numInputs)];
			(inputArray.size != numInputs).if { Error("bug: input array size mismatch!").throw };
			inputArray.do { arg item, i;
				(item.rate != \audio).if {
					Error("input % (%) is not audio rate".format(i, item)).throw;
				};
			};
		};
		offset = offset + 1 + numInputs;
		// parameter controls
		numParams = args[offset];
		(numParams > 0).if {
			paramArray = args[(offset+1)..(offset+(numParams*2))];
			(paramArray.size != (numParams*2)).if { Error("bug: param array size mismatch!").throw };
		};
		offset = offset + 1 + (numParams*2);
		// aux inputs
		numAuxInputs = args[offset];
		(numAuxInputs > 0).if {
			auxInputArray = args[(offset+1)..(offset+numAuxInputs)];
			(auxInputArray.size != numAuxInputs).if { Error("bug: aux input array size mismatch!").throw };
			auxInputArray.do { arg item, i;
				(item.rate != \audio).if {
					Error("aux input % (%) is not audio rate".format(i, item)).throw;
				};
			};
		};
		// substitute parameter names with indices
		paramArray.pairsDo { arg param, value, i;
			param.isNumber.not.if {
				info ?? { ^Error("can't resolve parameter '%' without info".format(param)).throw; };
				sym = param.asSymbol;
				param = info.findParamIndex(sym);
				param ?? { ^Error("Bad parameter '%' for plugin '%'".format(sym, info.name)).throw; };
				paramArray[i] = param;
			};
		};
		// reassemble UGen inputs (in correct order)
		inputs = [numOut, flags, bypass, numInputs] ++ inputArray
		    ++ numAuxInputs ++ auxInputArray ++ numParams ++ paramArray;
		^this.initOutputs(numOut + numAuxOut, rate)
	}
}
