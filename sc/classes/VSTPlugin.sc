VSTPlugin : MultiOutUGen {
	// class members
	classvar <versionMajor=0;
	classvar <versionMinor=4;
	classvar <versionBugfix=1;
	classvar pluginDict;
	classvar <platformExtension;
	// instance members
	var <>id;
	var <>info;
	var <>desc;
	// class methods
	*initClass {
		StartUp.add {
			platformExtension = Platform.case(
				\windows, ".dll", \osx, ".vst", \linux, ".so"
			);
			pluginDict = IdentityDictionary.new;
			pluginDict[Server.default] = IdentityDictionary.new;
		}
	}
	*ar { arg input, numOut=1, bypass=0, params, id, info, auxInput, numAuxOut=0, blockSize;
		input = input.asArray;
		auxInput = auxInput.asArray;
		params = params.asArray;
		params.size.odd.if {
			MethodError("'params' must be pairs of param index/name + value", this).throw;
		};
		^this.multiNewList([\audio, id, info, numOut, numAuxOut, blockSize, bypass, input.size]
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
		this.clear(server);
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
		(dir.isNil or: dir.isArray).not.if { MethodError("bad type % for 'dir' argument!".format(dir.class), this).throw };
		dir = dir.collect({ arg p; p.asString.standardizePath});
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
					this.prAddPlugin(dict, info.key, info);
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
						this.prAddPlugin(dict, info.key, info);
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
		path = path.asString.standardizePath;
		// add dictionary if it doesn't exist yet
		pluginDict[server].isNil.if { pluginDict[server] = IdentityDictionary.new };
		server.isLocal.if { this.prProbeLocal(server, path, key, action); }
		{ this.prProbeRemote(server, path, key, wait, action); };
	}
	*probeMsg { arg path, dest=nil;
		path = path.asString.standardizePath;
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
					info = VSTPluginDesc.prParse(stream).scanPresets;
					// store under key
					this.prAddPlugin(dict, info.key, info);
					// also store under resolved path and custom key
					this.prAddPlugin(dict, path, info);
					key !? {
						this.prAddPlugin(dict, key, info);
					}
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
						info = VSTPluginDesc.prParse(CollStream.new(string)).scanPresets;
						// store under key
						this.prAddPlugin(dict, info.key, info);
						// also store under resolved path and custom key
						this.prAddPlugin(dict, path, info);
						key !? {
							this.prAddPlugin(dict, key, info);
						}
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
				this.prAddPlugin(dict, info.key, info);
			};
		};
		^dict;
	}
	*prAddPlugin { arg dict, key, info;
		key = key.asSymbol;
		// we prefer non-bridged plugins, so we don't overwrite
		// an existing non-bridged plugin with a new bridged plugin.
		dict[key] !? {
			(dict[key].bridged.not && info.bridged).if { ^this; }
		};
		dict[key] = info;
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
		onset ?? { Error("expecting 'n=<number>'").throw; };
		^line[(onset+1)..].asInteger; // will eat whitespace and stop at newline
	}
	*prParseKeyValuePair { arg line;
		var key, value, split = line.find("=");
		split ?? { Error("expecting 'key=value'").throw; };
		key = line[0..(split-1)];
		split = split + 1;
		(split < line.size).if {
			value = line[split..];
		} { value = "" };
		^[key.stripWhiteSpace.asSymbol, value.stripWhiteSpace ];
	}
	*prParseIni { arg stream;
		var results, onset, line, n, indices, last = 0;
		var major = 0, minor = 0, bugfix = 0;
		// get version
		line = this.prGetLine(stream, true);
		(line == "[version]").if {
			#major, minor, bugfix = this.prGetLine(stream).split($.).collect(_.asInteger);
			line = this.prGetLine(stream, true);
		};
		(line != "[plugins]").if { Error("missing [plugins] header").throw };
		// get number of plugins
		line = this.prGetLine(stream, true);
		n = this.prParseCount(line);
		results = Array.newClear(n);
		// now deserialize plugins
		n.do { arg i;
			results[i] = VSTPluginDesc.prParse(stream, major, minor, bugfix).scanPresets;
		};
		^results;
	}
	*prGetInfo { arg server, key, wait, action;
		var info, dict = pluginDict[server];
		key = key.asSymbol;
		// if the key already exists, return the info.
		// otherwise probe the plugin and store it under the given key
		dict !? { info = dict[key] } !? { action.value(info) }
		?? { this.probe(server, key, key, wait, action) };
	}
	*prMakeTmpPath {
		^PathName.tmp +/+ "vst_" ++ UniqueID.next;
	}
	*prMakeDest { arg dest;
		// 'dest' may be a) temp file path, b) bufnum, c) Buffer, d) nil
		dest !? {
			(dest.isString).if {
				^dest.standardizePath; // tmp file path
			};
			dest = dest.asUGenInput;
			dest.isNumber.if { ^dest.asInteger }; // bufnum
			Error("bad type '%' for 'dest' argument!".format(dest.class)).throw;
		}
		^-1; // invalid bufnum: don't write results
	}

	// instance methods
	init { arg id, info, numOut, numAuxOut, blockSize, bypass ... args;
		var numInputs, inputArray, numParams, paramArray, numAuxInputs, auxInputArray, offset=0;
		// store id and info (both optional)
		this.id = id !? { id.asSymbol }; // !
		this.info = (info.notNil && info.isKindOf(VSTPluginDesc).not).if {
			// try to get plugin from default server
			VSTPlugin.plugins[info.asSymbol] ?? { MethodError("can't find plugin '%' (did you forget to call VSTPlugin.search?)".format(info), this).throw; };
		} { info };
		// main inputs
		numInputs = args[offset];
		(numInputs > 0).if {
			inputArray = args[(offset+1)..(offset+numInputs)];
			(inputArray.size != numInputs).if { MethodError("bug: input array size mismatch!", this).throw };
			inputArray.do { arg item, i;
				(item.rate != \audio).if {
					MethodError("input % (%) is not audio rate".format(i, item), this).throw;
				};
			};
		};
		offset = offset + 1 + numInputs;
		// parameter controls
		numParams = args[offset];
		(numParams > 0).if {
			paramArray = args[(offset+1)..(offset+(numParams*2))];
			(paramArray.size != (numParams*2)).if { MethodError("bug: param array size mismatch!", this).throw };
		};
		offset = offset + 1 + (numParams*2);
		// aux inputs
		numAuxInputs = args[offset];
		(numAuxInputs > 0).if {
			auxInputArray = args[(offset+1)..(offset+numAuxInputs)];
			(auxInputArray.size != numAuxInputs).if { MethodError("bug: aux input array size mismatch!", this).throw };
			auxInputArray.do { arg item, i;
				(item.rate != \audio).if {
					MethodError("aux input % (%) is not audio rate".format(i, item), this).throw;
				};
			};
		};
		// substitute parameter names with indices
		paramArray.pairsDo { arg param, value, i;
			var index;
			param.isValidUGenInput.not.if {
				(param.isString || param.isKindOf(Symbol)).if {
					this.info ?? { MethodError("can't resolve parameter '%' without info".format(param), this).throw; };
					index = this.info.findParamIndex(param.asSymbol);
					index ?? { MethodError("unknown parameter '%' for plugin '%'".format(param, this.info.name), this).throw; };
					paramArray[i] = index;
				} {
					MethodError("bad parameter index '%'".format(param), this).throw;
				}
			};
		};
		// reassemble UGen inputs (in correct order)
		inputs = [numOut, blockSize ?? { 0 }, bypass, numInputs] ++ inputArray
		    ++ numAuxInputs ++ auxInputArray ++ numParams ++ paramArray;
		^this.initOutputs(numOut + numAuxOut, rate);
	}
	optimizeGraph {
		// This is called exactly once during SynthDef construction!
		var metadata;
		// For older SC versions, where metadata might be 'nil'
		this.synthDef.metadata ?? { this.synthDef.metadata = () };
		// Add vstplugin metadata entry if needed:
		metadata = this.synthDef.metadata[\vstplugins];
		metadata ?? {
			metadata = ();
			this.synthDef.metadata[\vstplugins] = metadata;
		};
		// Make plugin description and add to metadata:
		this.desc = ();
		this.info !? { this.desc[\key] = this.info.key };
		// There can only be a single VSTPlugin without ID. In this case, the metadata will contain
		// a (single) item at the pseudo key 'false', see VSTPluginController.prFindPlugins.
		this.id.notNil.if {
			// check for VSTPlugin without ID
			metadata.at(false).notNil.if {
				Error("SynthDef '%' contains multiple VSTPlugin instances - can't omit 'id' argument!".format(this.synthDef.name)).throw;
			};
			// check for duplicate ID
			metadata.at(this.id).notNil.if {
				Error("SynthDef '%' contains duplicate VSTPlugin ID '%'".format(this.synthDef.name, this.id)).throw;
			};
			metadata.put(this.id, this.desc);
		} {
			// metadata must not contain other VSTPlugins!
			(metadata.size > 0).if {
				Error("SynthDef '%' contains multiple VSTPlugin instances - can't omit 'id' argument!".format(this.synthDef.name)).throw;
			};
			metadata.put(false, this.desc);
		};
	}
	synthIndex_ { arg index;
		super.synthIndex_(index); // !
		// update metadata (ignored if reconstructing from disk)
		this.desc.notNil.if { this.desc.index = index; }
	}
}
