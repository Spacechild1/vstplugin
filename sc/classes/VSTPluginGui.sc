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
		if (browser.notNil) { browser.close };
		// always notify when changing models
		if (model.notNil) {
			this.prClose;
			model.removeDependant(this);
		};
		model = newModel;
		if (model.notNil) {
			model.addDependant(this);
			this.prOpen;
		} {
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
		var name, infoString, header, browse, nparams=0, nrows=0, ncolumns=0;
		var layout, menuLayout, font, displayFont;
		var numRows = this.numRows ?? this.class.numRows;
		var displayWidth = this.displayWidth ?? this.class.displayWidth;
		var menu = this.menu ?? this.class.menu;
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

		if (menu) {
			this.prAddPresetMenu(menuLayout);
			this.prUpdatePresets;
		} {
			presetMenu = nil; updateButtons = nil;
		};
		layout.add(menuLayout);

		// build parameters
		if (menu && showParams) {
			layout.add(
				this.prCreateParams(info, nparams, nrows, ncolumns, displayWidth, displayFont);
			)
		} {
			paramSliders = nil;
			paramDisplays = nil;
			layout.add(nil);
		};
		view.layout_(layout);
	}

	prAddPresetMenu { arg layout;
		var saveButton, saveAsButton, deleteButton;
		var renameButton, reloadButton, textField;

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
		saveButton = Button.new.states_([["Save"]])
		.action_({
			var item = presetMenu.item;
			(item.notNil and: { item.type == \preset }).if {
				model.savePreset(item.index, async: true);
			} { Error("Save button bug").throw }
		}).enabled_(false);
		// "save as" button
		saveAsButton = Button.new.states_([["Save As"]])
		.action_({ arg self;
			textField.value(self, { arg name;
				model.savePreset(name, async: true);
			});
		});
		// "rename" button
		renameButton = Button.new.states_([["Rename"]])
		.action_({ arg self;
			var item = presetMenu.item;
			(item.notNil and: { item.type == \preset }).if {
				textField.value(self, { arg name;
					model.renamePreset(item.index, name);
				}, item.preset.name);
			} { Error("Rename button bug").throw }
		}).enabled_(false);
		// "delete" button
		deleteButton = Button.new.states_([["Delete"]])
		.action_({
			var item = presetMenu.item;
			(item.notNil and: { item.type == \preset }).if {
				model.deletePreset(item.index);
			} { Error("Delete button bug").throw }
		}).enabled_(false);
		// "reload" button
		reloadButton = Button.new.states_([["Reload"]])
		.action_({
			var item = presetMenu.item;
			(item.notNil and: { item.type == \preset }).if {
				model.loadPreset(item.index, async: true);
			} { Error("Reload button bug").throw }
		}).enabled_(false);

		updateButtons = {
			var enable = false;
			var item = (presetMenu.items.size > 0).if {
				// 'item' throws if 'items' is empty
				presetMenu.item
			};
			(item.notNil and: { item.type == \preset }).if {
				enable = item.preset.type == \user;
			};
			saveButton.enabled_(enable);
			renameButton.enabled_(enable);
			deleteButton.enabled_(enable);
			// reloading built-in presets doesn't work with all plugins...
			reloadButton.enabled_(enable);
		};

		layout.add(saveButton, 1, 0).add(saveAsButton, 1, 1)
		.add(renameButton, 1, 2).add(deleteButton, 1, 3)
		.addSpanning(presetMenu, 2, 0, 1, 3).add(reloadButton, 2, 3);
	}

	prCreateParams { arg info, numParams, numRows, numColumns, displayWidth, font;
		var sliderWidth = this.sliderWidth ?? this.class.sliderWidth;
		var sliderHeight = this.sliderHeight ?? this.class.sliderHeight;

		var paramLayout = GridLayout.new.spacing_(12);
		paramSliders = Array.new(numParams);
		paramDisplays = Array.new(numParams);
		numParams.do { arg i;
			var name, label, display, slider, bar, unit;
			var param = info.parameters[i];
			var state = model.parameterCache[i];
			var col = i.div(numRows);
			var row = i % numRows;
			// param name
			name = StaticText.new
			.string_("%: %".format(i, param.name));
			// param label
			label = (param.label.size > 0).if { StaticText.new.string_(param.label) };
			// param display
			display = TextField.new
			.fixedWidth_(displayWidth).font_(font).string_(state[1]);
			display.action = {arg s; model.set(i, s.value)};
			paramDisplays = paramDisplays.add(display);
			// slider
			slider = Slider.new(bounds: sliderWidth@sliderHeight)
			.fixedSize_(sliderWidth@sliderHeight).value_(state[0]);
			slider.action = {arg s; model.set(i, s.value)};
			paramSliders = paramSliders.add(slider);
			// put together
			bar = HLayout.new([name.align_(\left), stretch: 1], display.align_(\right)).spacing_(5);
			label !? { bar.add(label) };
			unit = VLayout.new(bar, slider).spacing_(5);
			paramLayout.add(unit, row, col);
		};
		// don't expand grid:
		paramLayout.setRowStretch(numRows, 1);
		paramLayout.setColumnStretch(numColumns, 1);
		// paramLayout.margins_([2, 12, 2, 2]);

		^ScrollView.new
		.hasHorizontalScroller_(true)
		.hasVerticalScroller_(true)
		.autohidesScrollers_(true)
		.canvas_(View.new.layout_(paramLayout));
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

	prBrowse {
		if (model.notNil) {
			// prevent opening the dialog multiple times
			if (browser.isNil) {
				browser = VSTPluginBrowser(\dialog, model.synth.server);
				browser.currentPlugin = model.info;
				browser.action = { |info, options|
					if (info.notNil) {
						// NB: in SC 3.14+ we could actually do the following:
						// this.performArgs(\open, [ info.key ], options.asKeyValuePairs);
						model.open(info.key, editor: options.editor ? true,
							multiThreading: options.multiThreading ? false,
							mode: options.mode);

					}
				};
				browser.onClose = { browser = nil };
			};
			browser.front;
		} { "no model!".error };
	}

	writeName {}
}

