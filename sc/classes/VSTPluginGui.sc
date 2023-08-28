VSTPluginGui : ObjectGui {
	// class defaults (can be overwritten per instance)
	classvar <>numRows = 10; // max. number of parameters per column
	classvar <>closeOnFree = true;
	classvar <>sliderWidth = 200;
	classvar <>sliderHeight = 20;
	classvar <>displayWidth = 7;
	classvar <>menu = true;
	// public
	var <>closeOnFree;
	var <>numRows;
	var <>sliderWidth;
	var <>sliderHeight;
	var <>displayWidth;
	var <>menu;
	// private
	classvar pluginPath;
	classvar defaultViewWidth = 400;
	classvar defaultViewHeight = 400;
	var server;
	var presetMenu;
	var updateButtons;
	var paramSliders;
	var paramDisplays;
	var embedded;
	var browser;
	var showParams;
	var info;

	model_ { arg newModel;
		// close the browser (if opened)
		browser !? { browser.close };
		// always notify when changing models
		model !? {
			this.prClose;
			model.removeDependant(this);
		};
		model = newModel;
		model.notNil.if {
			model.addDependant(this);
			server = model.synth.server;
			this.prOpen;
		} {
			server = Server.default;
			this.prUpdateGui;
		}
	}

	// this is called whenever something important in the model changes.
	update { arg who, what ...args;
		{
			who.notNil.if {
				switch(what,
					\open, { this.prOpen },
					\close, { this.prClose },
					\free, { this.prFree }, // Synth has been freed
					\param, { this.prParamChanged(*args) },
					\program_name, { this.prUpdatePresets },
					\program_index, { this.prProgramIndex(*args) },
					\presets, { this.prUpdatePresets },
					\preset_load, { this.prPresetSelect(*args) },
					\preset_save, { this.prPresetSelect(*args) }
				)
			} {
				// empty update call
				this.prUpdateGui;
			}
		}.defer;
	}

	prOpen {
		// unregister from old info
		info !? { info.removeDependant(this) };
		// register to new info
		info = model.info;
		info.addDependant(this);
		this.prUpdateGui;
	}

	prClose {
		info !? { info.removeDependant(this); info = nil };
		this.prUpdateGui;
	}

	prFree {
		(this.closeOnFree ?? this.class.closeOnFree).if {
			embedded.not.if {
				view !? { view.close };
				^this;
			};
		};
		this.prClose;
	}

	guify { arg parent, bounds, params=true;
		bounds !? {
			bounds = bounds.asRect;
		};
		parent.isNil.if {
			bounds ?? {
				params.if {
					bounds = defaultViewWidth@defaultViewHeight;
				}{
					bounds = 10@10; // hack to expand automatically
				};
				bounds = bounds.asRect.center_(Window.availableBounds.center);
			};
			parent = Window(bounds: bounds).asView;
		} {
			bounds ?? {
				params.if {
					bounds = defaultViewWidth@defaultViewHeight;
				} {
					bounds = defaultViewWidth@100; // empirically
				}
			};
			parent = View(parent, bounds);
		};
		// notify the GUI on close to release its dependencies!
		parent.addAction({ this.viewDidClose }, 'onClose');
		^parent
	}

	gui { arg parent, bounds, params=true;
		showParams = params;
		embedded = parent.notNil;
		view = this.guify(parent, bounds, params);
		this.prUpdateGui;
		// window
		embedded.not.if { view.front };
	}

	viewDidClose {
		browser !? { browser.close };
		info !? { info.removeDependant(this); info = nil };
		super.viewDidClose;
	}

	prUpdateGui {
		var nparams=0, name, infoString, header, browse, nrows=0, ncolumns=0;
		var layout, menuLayout, paramView, paramLayout, font, displayFont;
		var numRows = this.numRows ?? this.class.numRows;
		var sliderWidth = this.sliderWidth ?? this.class.sliderWidth;
		var sliderHeight = this.sliderHeight ?? this.class.sliderHeight;
		var displayWidth = this.displayWidth ?? this.class.displayWidth;
		var menu = this.menu ?? this.class.menu;
		var save, saveas, delete, rename, reload, textField;
		// displayWidth is measured in characters, so use a monospace font.
		// use point size to adapt to different screen resolutions
		displayFont = Font.new(Font.defaultMonoFace, 10, usePointSize: true);
		// get the max. display width in pixels (use an extra character for safety)
		displayWidth = String.fill(displayWidth + 1, $0).bounds(displayFont).width;
		// remove old GUI body or return if we don't have a view ('gui' hasn't been called)
		view.notNil.if { view.removeAll } { ^this };
		info.notNil.if {
			name = info.name;
			infoString = info.prToString;
			menu = menu.asBoolean;
			// parameters: calculate number of rows and columns
			showParams.if {
				nparams = model.numParameters;
				ncolumns = nparams.div(numRows) + ((nparams % numRows) != 0).asInteger;
				(ncolumns == 0).if {ncolumns = 1}; // just to prevent division by zero
				nrows = nparams.div(ncolumns) + ((nparams % ncolumns) != 0).asInteger;
			}
		} { menu = false };

		font = Font.new(*GUI.skin.fontSpecs).pointSize_(12);
		// change window header
		embedded.not.if {
			view.name_(name !? { "VSTPlugin (%)".format(name) } ?? { "VSTPlugin (empty)" });
		};

		header = StaticText.new
		.font_(font)
		// .stringColor_(GUI.skin.fontColor)
		// .background_(GUI.skin.background)
		.align_(\left)
		.object_(model !? { name ?? "[no plugin]" } ?? "[no model]")
		.toolTip_(infoString);
		// "Browse" button
		model !? {
			browse = Button.new
			.states_([["Browse"]])
			.action_({this.prBrowse})
			.toolTip_("Browse plugins");
		};

		layout = VLayout.new;
		// "Browse" button + plugin name spanning 4 cells + an expanding extra cell
		menuLayout = GridLayout.new
		.add(browse, 0, 0)
		.addSpanning(header, 0, 1, 1, 4)
		.setColumnStretch(4, 1);

		menu.if {
			// build preset menu
			presetMenu = PopUpMenu.new
			.action = { arg self;
				var item = self.item;
				item.notNil.if {
					(item.type == \program).if {
						model.program_(item.index);
					} {
						model.loadPreset(item.index, async: true);
					}
				};
				updateButtons.value;
			};

			textField = { arg parent, action, name;
				var pos = parent.absoluteBounds.origin;
				TextField.new(bounds: Rect.new(pos.x, pos.y, 200, 30))
				.name_("Preset name")
				.string_(name)
				.action_({ arg self;
					// Return key pressed
					(self.string.size > 0).if {
						action.value(self.string);
					};
					self.close
				})
				.front;
			};

			// "save" button
			save = Button.new.states_([["Save"]])
			.action_({
				var item = presetMenu.item;
				(item.notNil and: { item.type == \preset }).if {
					model.savePreset(item.index, async: true);
				} { Error("Save button bug").throw }
			}).enabled_(false);
			// "save as" button
			saveas = Button.new.states_([["Save As"]])
			.action_({ arg self;
				textField.value(self, { arg name;
					model.savePreset(name, async: true);
				});
			});
			// "rename" button
			rename = Button.new.states_([["Rename"]])
			.action_({ arg self;
				var item = presetMenu.item;
				(item.notNil and: { item.type == \preset }).if {
					textField.value(self, { arg name;
						model.renamePreset(item.index, name);
					}, item.preset.name);
				} { Error("Rename button bug").throw }
			}).enabled_(false);
			// "delete" button
			delete = Button.new.states_([["Delete"]])
			.action_({
				var item = presetMenu.item;
				(item.notNil and: { item.type == \preset }).if {
					model.deletePreset(item.index);
				} { Error("Delete button bug").throw }
			}).enabled_(false);
			// "reload" button
			reload = Button.new.states_([["Reload"]])
			.action_({
				var item = presetMenu.item;
				(item.notNil and: { item.type == \preset }).if {
					model.loadPreset(item.index, async: true);
				} { Error("Reload button bug").throw }
			}).enabled_(false);

			updateButtons = {
				var enable = false;
				var item = (presetMenu.items.size > 0).if { presetMenu.item }; // 'item' throws if 'items' is empty
				(item.notNil and: { item.type == \preset }).if {
					enable = item.preset.type == \user;
				};
				save.enabled_(enable);
				rename.enabled_(enable);
				delete.enabled_(enable);
				reload.enabled_(enable); // reloading built-in presets doesn't work with all plugins...
			};

			menuLayout.add(save, 1, 0).add(saveas, 1, 1).add(rename, 1, 2).add(delete, 1, 3)
			.addSpanning(presetMenu, 2, 0, 1, 3).add(reload, 2, 3);

			this.prUpdatePresets;
		} {
			presetMenu = nil; updateButtons = nil;
		};

		layout.add(menuLayout);

		// build parameters
		(menu && showParams).if {
			paramLayout = GridLayout.new.spacing_(12);
			paramSliders = Array.new(nparams);
			paramDisplays = Array.new(nparams);
			nparams.do { arg i;
				var param, row, col, name, label, display, slider, bar, unit, state;
				param = info.parameters[i];
				state = model.parameterCache[i];
				col = i.div(nrows);
				row = i % nrows;
				// param name
				name = StaticText.new
				.string_("%: %".format(i, param.name));
				// param label
				label = (param.label.size > 0).if { StaticText.new.string_(param.label) };
				// param display
				display = TextField.new
				.fixedWidth_(displayWidth).font_(displayFont).string_(state[1]);
				display.action = {arg s; model.set(i, s.value)};
				paramDisplays.add(display);
				// slider
				slider = Slider.new(bounds: sliderWidth@sliderHeight)
				.fixedSize_(sliderWidth@sliderHeight).value_(state[0]);
				slider.action = {arg s; model.set(i, s.value)};
				paramSliders.add(slider);
				// put together
				bar = HLayout.new([name.align_(\left), stretch: 1], display.align_(\right)).spacing_(5);
				label !? { bar.add(label) };
				unit = VLayout.new(bar, slider).spacing_(5);
				paramLayout.add(unit, row, col);
			};
			// don't expand grid:
			paramLayout.setRowStretch(nrows, 1);
			paramLayout.setColumnStretch(ncolumns, 1);
			// paramLayout.margins_([2, 12, 2, 2]);

			paramView = ScrollView.new
			.hasHorizontalScroller_(true)
			.hasVerticalScroller_(true)
			.autohidesScrollers_(true)
			.canvas_(View.new.layout_(paramLayout));

			layout.add(paramView);
		} {
			paramSliders = nil;
			paramDisplays = nil;
			layout.add(nil);
		};
		view.layout_(layout);
	}

	prParamChanged { arg index, value, display;
		paramSliders.notNil.if {
			paramSliders[index].value_(value);
			paramDisplays[index].string_(display);
		}
	}

	prProgramIndex { arg index;
		presetMenu.notNil.if {
			presetMenu.value_(index + 1); // skip label
			updateButtons.value;
		}
	}

	prUpdatePresets {
		var oldpreset, oldindex, presets, sorted, labels = [], items = [];
		(presetMenu.notNil && model.notNil).if {
			oldindex = presetMenu.value;
			oldpreset = (oldindex.notNil and:
				{ presetMenu.item.notNil and: { presetMenu.item.type == \preset }}).if {
				presetMenu.item.preset;
			};
			(info.numPrograms > 0).if {
				// append programs
				labels = labels.add("--- built-in programs ---");
				items = items.add(nil);
				model.programCache.do { arg name, i;
					labels = labels.add(name);
					items = items.add((type: \program, index: i));
				}
			};
			(info.numPresets > 0).if {
				presets = info.presets;
				// collect preset indices by type
				sorted = (user: List.new, userFactory: List.new, sharedFactory: List.new, global: List.new);
				presets.do { arg preset, i;
					sorted[preset.type].add(i);
				};
				#[
					\user, "--- user presets ---",
					\userFactory, "--- user factory presets ---",
					\sharedFactory, "--- shared factory presets ---",
					\global, "--- global presets ---"
				].pairsDo { arg type, label;
					(sorted[type].size > 0).if {
						// add label
						labels = labels.add(label);
						items = items.add(nil);
						// add presets
						sorted[type].do { arg index;
							labels = labels.add(presets[index].name);
							items = items.add((type: \preset, index: index, preset: presets[index]));
						}
					};
				};
			};
			// set labels and replace items
			presetMenu.items_(labels);
			items.do { arg item, i;
				presetMenu.items[i] = item;
			};
			// check if preset count has changed
			oldpreset.notNil.if {
				// try to find old preset (if not found, the index will remain 0)
				presetMenu.items.do { arg item, index;
					(item.notNil and: { item.preset == oldpreset }).if {
						presetMenu.value_(index);
					}
				}
			} {
				// simply restore old index
				oldindex.notNil.if { presetMenu.value_(oldindex) }
			};
			updateButtons.value;
			presetMenu.focus(true); // hack
		}
	}

	prPresetSelect { arg preset;
		(presetMenu.notNil && model.notNil).if {
			presetMenu.items.do { arg item, index;
				item.notNil.if {
					((item.type == \preset) and: { item.index == preset }).if {
						presetMenu.value_(index);
						updateButtons.value;
						^this;
					}
				}
			}
		}
	}

	*prMakePluginBrowser { arg model, settings;
		var window, browser, dir, file, clear, editor, multiThreading, mode, search, path, ok, cancel, status, key, absPath;
		var showSearch, updatePlugins, plugins, filteredPlugins, server;
		var applyFilter, stringFilter, vendorFilter, categoryFilter, vst2Filter, vst3Filter, fxFilter, synthFilter, showBridged;
		server = model.synth.server;
		window = Window.new.alwaysOnTop_(true).name_("VST plugin browser");
		browser = ListView.new.selectionMode_(\single);
		// called when a plugin is selected
		browser.action = {
			var info;
			browser.value !? { info = filteredPlugins[browser.value] };
			info.notNil.if {
				key = info.key;
				absPath = info.path;
				browser.toolTip_(info.prToString);
			} { key = nil; absPath = nil };
			showSearch.(false);
		};
		// called when one of the filters change
		applyFilter = {
			var items;
			filteredPlugins = plugins.select({ arg item;
				var ok = true, vst3 = item.sdkVersion.find("VST 3").notNil;
				var phrase = stringFilter.string.toLower;
				(phrase.size > 0).if {
					// search plugin and vendor name
					ok = item.name.toLower.find(phrase).notNil or: { item.vendor.toLower.find(phrase).notNil };
				};
				// use shortcircuiting to skip test if 'ok' is already 'false'
				ok = ok and: {
					(vst2Filter.value && vst3.not) ||
					(vst3Filter.value && vst3)
				};
				ok = ok and: {
					(fxFilter.value && item.synth.not) ||
					(synthFilter.value && item.synth)
				};
				ok = ok and: {
					item.bridged.not || showBridged.value
				};
				(vendorFilter.value > 0).if {
					ok = ok and: {
						(item.vendor.size > 0).if {
							item.vendor == vendorFilter.item;
						} {
							vendorFilter.item == "[unknown]";
						}
					};
				};
				(categoryFilter.value > 0).if {
					ok = ok and: {
						item.category.split($|).indexOfEqual(categoryFilter.item).notNil;
					}
				};
				ok;
			});
			items = filteredPlugins.collect({ arg item;
				var vendor = (item.vendor.size > 0).if { item.vendor } { "unknown" };
				var bridged = item.bridged.if { "[bridged]" } { "" };
				"% (%) %".format(item.key, vendor, bridged); // rather use key instead of name
			});
			browser.toolTip_(nil);
			browser.items = items;
			// restore current plugin
			key !? {
				filteredPlugins.do { arg item, index;
					(item.key == key).if { browser.value_(index) }
				}
			};
			// manually call action
			browser.action.value;
		};
		// called after a new search
		updatePlugins = {
			var categories = Set.new;
			var vendors = Set.new;
			var oldCategory = categoryFilter.item;
			var oldVendor = vendorFilter.item;
			plugins = VSTPlugin.pluginList(server, sorted: true);
			plugins.do({ arg item;
				vendors.add((item.vendor.size > 0).if { item.vendor } { "[unknown]" });
				item.category.split($|).do { arg cat; categories.add(cat) };
			});
			categoryFilter.items = ["All"] ++ categories.asArray.sort({ arg a, b; a.compare(b, true) < 0});
			vendorFilter.items = ["All"] ++ vendors.asArray.sort({ arg a, b; a.compare(b, true) < 0});
			// restore filters
			oldCategory.notNil.if {
				categoryFilter.items.do { arg item, index;
					(item == oldCategory).if { categoryFilter.value_(index) }
				}
			};
			oldVendor.notNil.if {
				vendorFilter.items.do { arg item, index;
					(item == oldVendor).if { vendorFilter.value_(index) }
				}
			};
			// now filter the plugins
			applyFilter.value;
		};

		// plugin filters
		// update on every key input; the delay makes sure we really see the updated text.
		stringFilter = TextField.new.minWidth_(60)
		.addAction({ AppClock.sched(0, applyFilter) }, 'keyDownAction');

		vst2Filter = CheckBox.new(text: "VST2").value_(true).action_(applyFilter);

		vst3Filter = CheckBox.new(text: "VST3").value_(true).action_(applyFilter);

		fxFilter = CheckBox.new(text: "FX").value_(true).action_(applyFilter);

		synthFilter = CheckBox.new(text: "Instrument").value_(true).action_(applyFilter);

		showBridged = CheckBox.new(text: "Show bridged plugins").value_(true).action_(applyFilter);

		vendorFilter = PopUpMenu.new.items_(["All"]).action_(applyFilter);

		categoryFilter = PopUpMenu.new.items_(["All"]).action_(applyFilter);

		// status bar
		status = StaticText.new.align_(\left);

		showSearch = { arg show;
			show.if { status.stringColor_(Color.red); status.string_("searching..."); } { status.string_("") };
		};

		// search buttons
		search = Button.new.states_([["Search"]])
		.toolTip_("Search for VST plugins in the platform specific default paths\n(see VSTPlugin*search)")
		.action_({
			showSearch.(true);
			VSTPlugin.search(server, verbose: true, action: {
				{ updatePlugins.value; }.defer;
			});
		});

		dir = Button.new.states_([["Directory"]])
		.toolTip_("Search a directory for VST plugins")
		.action_({
			FileDialog.new({ arg d;
				showSearch.(true);
				VSTPlugin.search(server, dir: d, verbose: true, action: {
					{ updatePlugins.value; }.defer;
				});
			}, nil, 2, 0, true, pluginPath);
		});

		file = Button.new.states_([["File"]])
		.toolTip_("Open a VST plugin file")
		.action_({
			FileDialog.new({ arg p;
				key = p;
				absPath = p;
				ok.action.value;
			}, nil, 1, 0, true, pluginPath);
		});

		clear = Button.new.states_([["Clear"]])
		.toolTip_("Clear the plugin cache")
		.action_({
			VSTPlugin.clear;
			updatePlugins.value;
		});

		// plugin options
		editor = CheckBox.new(text: "Editor").value_(true);

		multiThreading = CheckBox.new(text: "Multi-threading");

		mode = PopUpMenu.new.items_(["normal", "sandbox", "bridge"]);

		// cancel/ok
		cancel = Button.new.states_([["Cancel"]])
		.action = { window.close };

		ok = Button.new.states_([["Open"]])
		.action = {
			var theMode = #[\auto, \sandbox, \bridge][mode.value];
			key !? {
				// open with key - not absPath!
				model.open(key, editor: editor.value, multiThreading: multiThreading.value, mode: theMode);
				pluginPath = absPath.dirname;
				window.close;
			};
		};

		window.layout_(VLayout(
			browser,
			HLayout(
				[StaticText.new.string_("Find:"), stretch: 0],
				[stringFilter, stretch: 1],
				[StaticText.new.string_("Vendor:"), stretch: 0],
				[vendorFilter, stretch: 1],
				[StaticText.new.string_("Category:"), stretch: 0],
				[categoryFilter, stretch: 1]
			),
			HLayout(
				vst2Filter, vst3Filter, fxFilter, synthFilter, nil, showBridged
			),
			HLayout(search, dir, file, clear, nil, status),
			8,
			HLayout(StaticText.new.string_("Mode:"), mode,
				editor, multiThreading, nil, cancel, ok)
		));

		// start at current plugin
		model.info.notNil.if { key = model.info.key };
		updatePlugins.value;
		^window;
	}

	prBrowse {
		model.notNil.if {
			// prevent opening the dialog multiple times
			browser.isNil.if {
				browser = VSTPluginGui.prMakePluginBrowser(model);
				browser.view.addAction({ browser = nil }, 'onClose');
			};
			browser.front;
		} { "no model!".error };
	}

	writeName {}
}

