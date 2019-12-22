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
	classvar presetPath;
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
					'/open', { this.prOpen },
					'/close', { this.prClose },
					'/free', { this.prFree }, // Synth has been freed
					'/param', { this.prParam(*args) },
					'/program_name', { this.prUpdatePresets },
					'/program_index', { this.prProgramIndex(*args) },
					'/presets', { this.prUpdatePresets },
					'/preset_load', { this.prPresetSelect(*args) },
					'/preset_save', { this.prPresetSelect(*args) }
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
				view.parent.close;
				^this;
			};
		};
		this.prClose;
	}

	guify { arg parent, bounds;
		bounds.notNil.if {
			bounds = bounds.asRect;
		};
		parent.isNil.if {
			parent = Window(bounds: bounds, scroll: true);
		} { parent = parent.asView };
		// notify the GUI on close to release its dependencies!
		parent.asView.addAction({ this.viewDidClose }, 'onClose');
		^parent
	}

	gui { arg parent, bounds, params=true;
		var numRows, sliderWidth, layout = this.guify(parent, bounds);
		showParams = params;
		parent.isNil.if {
			view = View(layout, bounds).background_(this.background);
			embedded = false;
		} {
			view = View.new(bounds: bounds);
			ScrollView(layout, bounds)
			.background_(this.background)
			.hasHorizontalScroller_(true)
			.autohidesScrollers_(true)
			.canvas_(view);
			embedded = true;
		};
		this.prUpdateGui;
		// window
		parent.isNil.if {
			bounds.isNil.if {
				numRows = this.numRows ?? this.class.numRows;
				sliderWidth = this.sliderWidth ?? this.class.sliderWidth;
				params.if {
					layout.setInnerExtent(sliderWidth * 2, numRows * 40);
				} {
					layout.setInnerExtent(sliderWidth * 1.5, 150);
				}
			};
			layout.front;
		};
	}

	viewDidClose {
		browser !? { browser.close };
		info !? { info.removeDependant(this); info = nil };
		super.viewDidClose;
	}

	prUpdateGui {
		var nparams=0, name, infoString, header, browse, nrows=0, ncolumns=0;
		var layout, grid, font, minWidth, minHeight, displayFont, makePanel;
		var numRows = this.numRows ?? this.class.numRows;
		var sliderWidth = this.sliderWidth ?? this.class.sliderWidth;
		var sliderHeight = this.sliderHeight ?? this.class.sliderHeight;
		var displayWidth = this.displayWidth ?? this.class.displayWidth;
		var menu = this.menu ?? this.class.menu;
		var save, saveas, delete, rename, textField;
		// displayWidth is measured in characters, so use a monospace font.
		// use point size to adapt to different screen resolutions
		displayFont = Font.new(Font.defaultMonoFace, 10, usePointSize: true);
		// get the max. display width in pixels (use an extra character for safety)
		displayWidth = String.fill(displayWidth + 1, $0).bounds(displayFont).width;
		// remove old GUI body or return if don't have a view ('gui' hasn't been called)
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
			view.parent.name_(name !? { "VSTPlugin (%)".format(name) } ?? { "VSTPlugin (empty)" });
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
			.maxWidth_(60)
			.action_({this.prBrowse})
			.toolTip_("Browse plugins");
		};

		layout = VLayout.new;
		layout.add(HLayout(browse, header));

		menu.if {
			// build preset menu
			presetMenu = PopUpMenu.new;

			textField = { arg parent, action, name;
				var pos = parent.absoluteBounds.origin;
				TextField.new(bounds: Rect.new(pos.x, pos.y, 200, 30))
				.name_("Preset name")
				.string_(name)
				.addAction({ arg self ... args;
					// Return key pressed
					(args[4] == 0x01000004).if {
						(self.string.size > 0).if {
							action.value(self.string);
						};
						self.close
					}
				}, 'keyDownAction')
				.front;
			};

			save = Button.new.states_([["Save"]]).action_({
				var item = presetMenu.item;
				(item.notNil and: { item.type == \preset }).if {
					model.savePreset(item.index);
				} { "Save button bug".throw }
			}).maxWidth_(60).enabled_(false);
			saveas = Button.new.states_([["Save As"]]).action_({ arg self;
				textField.value(self, { arg name;
					model.savePreset(name);
				});
			}).maxWidth_(60);
			rename = Button.new.states_([["Rename"]]).action_({ arg self;
				var item = presetMenu.item;
				(item.notNil and: { item.type == \preset }).if {
					textField.value(self, { arg name;
						model.renamePreset(item.index, name);
					}, item.preset.name);
				} { "Rename button bug".throw }
			}).maxWidth_(60).enabled_(false);
			delete = Button.new.states_([["Delete"]]).action_({
				var item = presetMenu.item;
				(item.notNil and: { item.type == \preset }).if {
					model.deletePreset(item.index);
				} { "Delete button bug".throw }
			}).maxWidth_(60).enabled_(false);

			presetMenu.action = {
				var item = presetMenu.item;
				item.notNil.if {
					(item.type == \program).if {
						model.program_(item.index);
					} {
						model.loadPreset(item.index);
					}
				};
				updateButtons.value;
			};

			updateButtons = {
				var enable = false;
				var item = (presetMenu.items.size > 0).if { presetMenu.item }; // 'item' throws if 'items' is empty
				(item.notNil and: { item.type == \preset }).if {
					enable = item.preset.type == \user;
				};
				save.enabled_(enable);
				rename.enabled_(enable);
				delete.enabled_(enable);
			};

			(ncolumns > 1).if {
				layout.add(HLayout(presetMenu, save, saveas, rename, delete, nil));
			} {
				layout.add(VLayout(presetMenu, HLayout(save, saveas, rename, delete, nil)));
			};

			this.prUpdatePresets;
		} {
			presetMenu = nil; updateButtons = nil;
		};

		// build parameters
		showParams.if {
			grid = GridLayout.new.spacing_(12);
			paramSliders = Array.new(nparams);
			paramDisplays = Array.new(nparams);
			nparams.do { arg i;
				var param, row, col, name, label, display, slider, bar, unit, state;
				param = info.parameters[i];
				state = model.paramCache[i];
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
				grid.add(unit, row, col);
			};
			grid.setRowStretch(nrows, 1);
			grid.setColumnStretch(ncolumns, 1);
			grid.margins_([2, 12, 2, 2]);
			layout.add(grid);
		} { layout.add(nil) };

		// make the canvas (view) large enough to hold all its contents.
		// somehow it can't figure out the necessary size itself...
		minWidth = ((sliderWidth + 20) * ncolumns).max(240);
		minHeight = ((sliderHeight * 3 * nrows) + 120).max(140); // empirically
		view.layout_(layout).fixedSize_(minWidth@minHeight);
	}

	prParam { arg index, value, display;
		showParams.if {
			paramSliders[index].value_(value);
			paramDisplays[index].string_(display);
		}
	}

	prProgramIndex { arg index;
		presetMenu !? {
			presetMenu.value_(index + 1); // skip label
			updateButtons.value;
		}
	}

	prUpdatePresets {
		var oldpreset, oldsize, oldindex, presets, sorted, labels = [], items = [];
		(presetMenu.notNil && model.notNil).if {
			oldsize = presetMenu.items.size;
			oldindex = presetMenu.value;
			oldpreset = (oldindex.notNil and:
				{ presetMenu.item.notNil and: { presetMenu.item.type == \preset }}).if {
				presetMenu.item.preset;
			};
			(info.numPrograms > 0).if {
				// append programs
				labels = labels.add("--- built-in programs ---");
				items = items.add(nil);
				model.programNames.do { arg name, i;
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
			(oldpreset.notNil and: { oldsize != presetMenu.items.size }).if {
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
		var window, browser, dir, file, editor, search, path, ok, cancel, status, key, absPath;
		var showPath, showSearch, updateBrowser, plugins, server;
		server = model.synth.server;
		window = Window.new.alwaysOnTop_(true).name_("VST plugin browser");
		browser = ListView.new.selectionMode_(\single);
		browser.action = {
			var info = plugins[browser.value];
			key = info.key;
			info.notNil.if {
				absPath = info.path;
				showPath.value;
			} { "bug: no info!".error; }; // should never happen
		};
		updateBrowser = {
			var items;
			plugins = VSTPlugin.pluginList(server, sorted: true);
			items = plugins.collect({ arg item;
				"% (%)".format(item.key, item.vendor); // rather use key instead of name
			});
			browser.items = items;
			browser.value !? { browser.action.value } ?? { showPath.value };
		};
		status = StaticText.new.align_(\left).string_("Path:");
		showPath = { status.stringColor_(Color.black);
			absPath !? { status.string_("Path:" + absPath) } ?? { status.string_("Path:") }
		};
		showSearch = { status.stringColor_(Color.red); status.string_("searching..."); };

		search = Button.new.states_([["Search"]]).maxWidth_(60)
		.toolTip_("Search for VST plugins in the platform specific default paths\n(see VSTPlugin*search)");
		search.action = {
			showSearch.value;
			VSTPlugin.search(server, verbose: true, action: {
				{ updateBrowser.value; }.defer;
			});
		};
		dir = Button.new.states_([["Directory"]]).maxWidth_(60)
		.toolTip_("Search a directory for VST plugins");
		dir.action = {
			FileDialog.new({ arg d;
				showSearch.value;
				VSTPlugin.search(server, dir: d, useDefault: false, verbose: true, action: {
					{ updateBrowser.value; }.defer;
				});
			}, nil, 2, 0, true, pluginPath);
		};
		file = Button.new.states_([["File"]]).maxWidth_(60)
		.toolTip_("Open a VST plugin file");
		file.action = {
			FileDialog.new({ arg p;
				absPath = p;
				key = absPath;
				showPath.value;
				ok.action.value;
			}, nil, 1, 0, true, pluginPath);
		};
		editor = CheckBox.new(text: "editor");

		cancel = Button.new.states_([["Cancel"]]).maxWidth_(60);
		cancel.action = { window.close };
		ok = Button.new.states_([["OK"]]).maxWidth_(60);
		ok.action = {
			key !? {
				// open with key - not absPath!
				model.open(key, editor: editor.value);
				pluginPath = absPath;
				window.close;
			};
		};

		window.layout_(VLayout(
			browser, status, HLayout(search, dir, file, editor, nil, cancel, ok)
		));

		updateBrowser.value;
		^window;
	}

	prBrowse {
		var window;
		model.notNil.if {
			// prevent opening the dialog multiple times
			browser !? { ^this };
			// create dialog
			window = VSTPluginGui.prMakePluginBrowser(model);
			window.view.addAction({ browser = nil }, 'onClose');
			browser = window;
			window.front;
		} { "no model!".error };
	}

	writeName {}
}

