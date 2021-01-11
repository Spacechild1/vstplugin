VSTPluginDesc {
	// public fields
	var <>key;
	var <>path;
	var <>name;
	var <>vendor;
	var <>category;
	var <>version;
	var <>sdkVersion;
	var <>id;
	var <>numInputs;
	var <>numOutputs;
	var <>numAuxInputs;
	var <>numAuxOutputs;
	var <>parameters;
	var <>programs;
	var <>presets;
	// flags
	var <>editor;
	var <>synth;
	var <>singlePrecision;
	var <>doublePrecision;
	var <>midiInput;
	var <>midiOutput;
	var <>sysexInput;
	var <>sysexOutput;
	var <>bridged;
	// private fields
	var <>prParamIndexMap;
	// legacy methods
	isSynth {
		this.deprecated(thisMethod, this.class.findMethod(\synth));
		^this.synth;
	}
	hasEditor {
		this.deprecated(thisMethod, this.class.findMethod(\editor));
		^this.editor;
	}
	// public methods
	numParameters { ^parameters.size; }
	numPrograms { ^programs.size; }
	numPresets { ^presets.size; }
	findParamIndex { arg name;
		^this.prParamIndexMap[name.asSymbol];
	}
	printOn { arg stream;
		stream.atLimit.not.if {
			stream << this.class.name << "( " << this.name << " )";
		}
	}
	print { arg long = false;
		"---".postln;
		this.prToString.postln;
		long.if {
			"".postln;
			"parameters (%):".format(this.numParameters).postln;
			this.printParameters;
			"".postln;
			"programs (%):".format(this.numPrograms).postln;
			this.printPrograms;
			"".postln;
			"presets (%):".format(this.numPresets).postln;
			this.printPresets;
		};
		"---".postln;
	}
	printParameters {
		this.parameters.do { arg param, i;
			var label;
			label = (param.label.size > 0).if { "(%)".format(param.label) };
			"[%] % %".format(i, param.name, label ?? "").postln;
		}
	}
	printPrograms {
		this.programs.do { arg pgm, i;
			"[%] %".format(i, pgm.name).postln;
		}
	}
	printPresets {
		// collect presets by type
		var result = (user: List.new, userFactory: List.new, sharedFactory: List.new, global: List.new);
		this.presets.do { arg preset, i;
			result[preset.type].add(preset);
		};
		// print results
		#[
			\user, "--- user presets ---",
			\userFactory, "--- user factory presets ---",
			\sharedFactory, "--- shared factory presets ---",
			\global, "--- global presets ---"
		].pairsDo { arg type, label;
			(result[type].size > 0).if {
				label.postln;
				result[type].do { arg p; p.name.postln };
			}
		};
	}
	scanPresets {
		var vst3 = this.sdkVersion.find("VST 3").notNil;
		presets.clear;
		[\user, \userFactory, \sharedFactory, \global].do { arg type;
			var folder = this.presetFolder(type);
			folder.notNil.if {
				PathName(folder).files.do { arg file;
					var preset = (
						name: file.fileNameWithoutExtension,
						path: file.fullPath,
						type: type
					);
					var ext = file.extension;
					((vst3 and: { ext == "vstpreset" }) or:
						{ vst3.not and: { (ext == "fxp") or: { ext == "FXP" } }}
					).if {
						presets = presets.add(preset);
					}
				}
			}
		};
		this.prSortPresets(false);
		this.changed(\presets);
	}
	presetFolder { arg type = \user;
		var folder, vst3 = this.sdkVersion.find("VST 3").notNil;
		Platform.case(
			\windows,
			{
				folder = switch(type,
					\user, "USERPROFILE".getenv +/+ "Documents",
					\userFactory, "APPDATA".getenv,
					\sharedFactory, "PROGRAMDATA".getenv
				);
				folder !? { folder = folder +/+ if(vst3, "VST3 Presets", "VST2 Presets") }
			},
			\osx,
			{
				folder = switch(type,
					\user, "~/Library/Audio/Presets",
					\sharedFactory, "/Library/Audio/Presets"
				)
			},
			\linux,
			{
				var vst = if(type == \user, ".") ++ if(vst3, "vst3", "vst");
				folder = switch(type,
					\user, "~",
					\sharedFactory, "/usr/local/share",
					\global, "/usr/share",
				);
				folder !? { folder = folder +/+ vst +/+ "presets"; }
			}
		);
		^folder !? {
			folder.standardizePath +/+ this.class.prBashPath(this.vendor) +/+ this.class.prBashPath(this.name);
		}
	}
	presetPath { arg name, type = \user;
		var vst3 = sdkVersion.find("VST 3").notNil;
		^this.presetFolder(type) +/+ this.class.prBashPath(name) ++ if(vst3, ".vstpreset", ".fxp");
	}
	findPreset { arg preset;
		preset.isNumber.if {
			^presets[preset.asInteger];
		} {
			preset = preset.asString;
			presets.do { arg p, i;
				(p.name == preset).if { ^p };
			};
		}
		^nil; // not found
	}
	prPresetIndex { arg preset;
		preset.isKindOf(Event).if {
			^presets.indexOf(preset);
		} {
			preset = preset.asString;
			presets.do { arg p, index;
				(p.name == preset).if { ^index }
			};
			^nil;
		}
	}
	prSortPresets { arg userOnly=true;
		var temp = (user: List.new, userFactory: List.new, sharedFactory: List.new, global: List.new);
		presets.do { arg p; temp[p.type].add(p) };
		userOnly.if {
			temp[\user].sort({ arg a, b; a.name.compare(b.name, true) < 0 });
		} {
			temp.do { arg l; l.sort({ arg a, b; a.name.compare(b.name, true) < 0 }) };
		};
		presets = [];
		[\user, \userFactory, \sharedFactory, \global].do { arg type;
			presets = presets.addAll(temp[type]);
		}
	}
	*prBashPath { arg path;
		var forbidden = IdentitySet[$/, $\\, $", $?, $*, $:, $<, $>, $|];
		^path.collect({ arg c;
			forbidden.findMatch(c).notNil.if { $_ } { c }
		});
	}
	addPreset { arg name, path;
		var index = 0, preset = (name: name, path: path ?? { this.presetPath(name) }, type: \user);
		presets.do { arg p, i;
			(preset.type == p.type).if {
				// check if preset exists
				(preset.name == p.name).if { ^i };
				// find lexicographically correct position
				(preset.name.compare(p.name, true) > 0).if { index = i + 1 }
			}
		};
		presets = presets.insert(index, preset);
		this.changed(\presets);
		^index;
	}
	deletePreset { arg preset;
		var result = preset.isKindOf(Event).if { preset } { this.findPreset(preset) };
		// can only remove user presets!
		result.notNil.if {
			(result.type == \user).if {
				File.delete(result.path).if {
					presets.remove(result);
					this.changed(\presets);
					^true;
				} {
					("couldn't delete preset file" + result.path).error;
				}
			} { "preset '%' is not writeable!".format(result.name).error; }
		} {	"couldn't find preset '%'".format(preset).error	}
		^false;
	}
	renamePreset { arg preset, name;
		var result = preset.isKindOf(Event).if { preset } { this.findPreset(preset) };
		var newPath = this.presetPath(name);
		// can only rename user presets!
		result.notNil.if {
			(result.type == \user).if {
				File.exists(newPath).not.if {
					try {
						File.copy(result.path, newPath);
					} {
						("couldn't create file" + result.path).error;
						^false;
					};
					// delete old file
					File.delete(result.path).not.if {
						("couldn't delete old preset file" + result.path).warn;
					};
					// update name + path
					result.name = name;
					result.path = newPath;
					this.prSortPresets;
					this.changed(\presets);
					^true;
				} { "preset '%' already exists!".format(name).error; }
			} { "preset '%' not writeable!".format(result.name).error }
		} {	"couldn't find preset '%'".format(preset).error	};
		^false;
	}
	// private methods
	*prParse { arg stream, versionMajor, versionMinor, versionBugfix;
		var info = VSTPluginDesc.new;
		var parameters, indexMap, programs, keys;
		var line, key, value, onset, n, f, flags, plugin = false;
		var hex2int = #{ arg str;
			str.toUpper.ascii.reverse.sum { arg c, i;
				(c >= 65).if { (c - 55) << (i * 4);	}
				{ (c - 48) << (i * 4); }
			}
		};
		var future = versionMajor.notNil.if {
			(versionMajor > VSTPlugin.versionMajor) ||
			((versionMajor == VSTPlugin.versionMajor) && (versionMinor > VSTPlugin.versionMinor))
		} { false };
		// default values:
		info.numAuxInputs = 0;
		info.numAuxOutputs = 0;
		info.presets = [];
		{
			line = VSTPlugin.prGetLine(stream, true);
			line ?? { Error("EOF reached").throw };
			// line.postln;
			switch(line,
				"[plugin]", { plugin = true; },
				"[parameters]",
				{
					line = VSTPlugin.prGetLine(stream);
					n = VSTPlugin.prParseCount(line);
					parameters = Array.newClear(n);
					indexMap = IdentityDictionary.new;
					n.do { arg i;
						var name, label;
						line = VSTPlugin.prGetLine(stream);
						#name, label = line.split($,);
						parameters[i] = (
							name: name.stripWhiteSpace,
							label: label.stripWhiteSpace
							// id (ignore)
							// more info later...
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
					line = VSTPlugin.prGetLine(stream);
					n = VSTPlugin.prParseCount(line);
					programs = Array.newClear(n);
					n.do { arg i;
						var name = VSTPlugin.prGetLine(stream);
						programs[i] = (name: name); // more info later
					};
					info.programs = programs;
				},
				"[keys]",
				{
					line = VSTPlugin.prGetLine(stream);
					n = VSTPlugin.prParseCount(line);
					keys = Array.newClear(n);
					n.do { arg i;
						keys[i] = VSTPlugin.prGetLine(stream);
					};
					// take the first (primary) key
					info.key = keys[0].asSymbol;
					// *** EXIT POINT ***
					^info;
				},
				{
					// plugin
					plugin.not.if {
						future.if {
							"VSTPluginDesc: unknown data in .ini file (%)".format(line).warn;
						} {
							Error("bad data (%)".format(line)).throw;
						}
					};
					#key, value = VSTPlugin.prParseKeyValuePair(line);
					switch(key,
						\path, { info.path = value },
						\name, { info.name = value },
						\vendor, { info.vendor = value },
						\category, { info.category = value },
						\version, { info.version = value },
						\sdkversion, { info.sdkVersion = value },
						\id, { info.id = value },
						\inputs, { info.numInputs = value.asInteger },
						\outputs, { info.numOutputs = value.asInteger },
						\auxinputs, { info.numAuxInputs = value.asInteger },
						\auxoutputs, { info.numAuxOutputs = value.asInteger },
						\pgmchange, {}, // ignore
						\bypass, {}, // ignore
						\flags,
						{
							f = hex2int.(value);
							flags = Array.fill(9, {arg i; ((f >> i) & 1).asBoolean });
							info.editor = flags[0];
							info.synth = flags[1];
							info.singlePrecision = flags[2];
							info.doublePrecision = flags[3];
							info.midiInput = flags[4];
							info.midiOutput = flags[5];
							info.sysexInput = flags[6];
							info.sysexOutput = flags[7];
							info.bridged = flags[8];
						},
						{
							future.if {
								"VSTPluginDesc: unknown key '%' in .ini file".format(key).warn;
							} {
								Error("bad key '%'".format(key)).throw;
							}
						}
					);
				},
			);
		}.loop;
	}
	prToString { arg sep = $\n;
		var s = "name: %".format(this.name) ++ sep
		++ "type: %%%".format(this.sdkVersion,
			this.synth.if { " (synth)" } { "" }, this.bridged.if { " [bridged]" } { "" }) ++ sep
		++ "path: %".format(this.path) ++ sep
		++ "vendor: %".format(this.vendor) ++ sep
		++ "category: %".format(this.category) ++ sep
		++ "version: %".format(this.version) ++ sep
		++ "input channels: %".format(this.numInputs) ++ sep
		++ ((this.numAuxInputs > 0).if { "aux input channels: %".format(this.numAuxInputs) ++ sep } {""})
		++ "output channels: %".format(this.numOutputs) ++ sep
		++ ((this.numAuxOutputs > 0).if { "aux output channels: %".format(this.numAuxOutputs) ++ sep } {""})
		++ "parameters: %".format(this.numParameters) ++ sep
		++ "programs: %".format(this.numPrograms) ++ sep
		++ "presets: %".format(this.numPresets) ++ sep
		++ "editor: %".format(this.editor) ++ sep
		// ++ "single precision: %".format(this.singlePrecision) ++ sep
		// ++ "double precision: %".format(this.doublePrecision) ++ sep
		++ "MIDI input: %".format(this.midiInput) ++ sep
		++ "MIDI output: %".format(this.midiOutput)
		// ++ "sysex input: %".format(this.sysexInput) ++ sep
		// ++ "sysex output: %".format(this.sysexOutput) ++ sep
		;
		^s;
	}
}
