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
	var programMenu;
	var paramSliders;
	var paramDisplays;
	var embedded;
	var dialog;

	model_ { arg newModel;
		// always notify when changing models
		model.removeDependant(this);
		model = newModel;
		model.addDependant(this);
		// close the browser (if opened)
		dialog !? { dialog.close };
		// only update if we have a view (i.e. -gui has been called)
		view.notNil.if {
			this.update;
		};
	}

	// this is called whenever something important in the model changes.
	update { arg who, what ...args;
		{
			(who == model).if {
				what.switch
				{ '/open'} { this.prUpdateGui }
				{ '/close' } { this.prUpdateGui }
				{ '/free' } { this.prFree } // Synth has been freed
				{ '/param' } { this.prParam(*args) }
				{ '/program' } { this.prProgram(*args) }
				{ '/program_index' } { this.prProgramIndex(*args) };
			};
			// empty update call
			who ?? { this.prUpdateGui };
		}.defer;
	}

	guify { arg parent, bounds;
		// converts the parent to a FlowView or compatible object
		// thus creating a window from nil if needed
		// registers to remove self as dependent on model if window closes
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

	gui { arg parent, bounds;
		var layout = this.guify(parent, bounds);
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
				var numRows = this.numRows ?? this.class.numRows;
				var sliderWidth = this.sliderWidth ?? this.class.sliderWidth;
				layout.setInnerExtent(sliderWidth * 2, numRows * 40);
			};
			layout.front;
		};
	}

	viewDidClose {
		dialog !? { dialog.close };
		super.viewDidClose;
	}

	prUpdateGui {
		var rowOnset, nparams=0, name, info, header, open, ncolumns=0, nrows=0;
		var grid, font, minWidth, minHeight, minSize, displayFont;
		var numRows = this.numRows ?? this.class.numRows;
		var sliderWidth = this.sliderWidth ?? this.class.sliderWidth;
		var sliderHeight = this.sliderHeight ?? this.class.sliderHeight;
		var displayWidth = this.displayWidth ?? this.class.displayWidth;
		var menu = this.menu ?? this.class.menu;
		// displayWidth is measured in characters, so use a monospace font.
		// use point size to adapt to different screen resolutions
		displayFont = Font.new(Font.defaultMonoFace, 10, usePointSize: true);
		// get the max. display width in pixels (use an extra character for safety)
		displayWidth = String.fill(displayWidth + 1, $0).bounds(displayFont).width;
		// remove old GUI body
		view !? { view.removeAll };
		(model.notNil and: { model.info.notNil}).if {
			name = model.info.name;
			info = model.info.toString;
			menu = menu.asBoolean;
			// parameters: calculate number of rows and columns
			nparams = model.numParameters;
			ncolumns = nparams.div(numRows) + ((nparams % numRows) != 0).asInt;
			(ncolumns == 0).if {ncolumns = 1}; // just to prevent division by zero
			nrows = nparams.div(ncolumns) + ((nparams % ncolumns) != 0).asInt;
		} { menu = false };

		font = Font.new(*GUI.skin.fontSpecs).pointSize_(12);
		// change window header
		embedded.not.if {
			view.parent.name_(name !? { "VSTPlugin (%)".format(name) } ?? { "VSTPlugin (empty)" });
		};

		header = StaticText.new
		.stringColor_(GUI.skin.fontColor)
		.font_(font)
		.background_(GUI.skin.background)
		.align_(\center)
		.object_(name ?? "[empty]")
		.toolTip_(info ?? "No plugin loaded");
		// "Open" button
		open = Button.new
		.states_([["Open"]])
		.maxWidth_(60)
		.action_({this.prOpen})
		.toolTip_("Open a plugin");

		grid = GridLayout.new;
		grid.add(header, 0, 0);
		menu.if {
			var row = 1, col = 0;
			var makePanel = { arg what;
				var label, read, write;
				label = StaticText.new.string_(what).align_(\right)
				.toolTip_("Read/write % files (.fx%)".format(what.toLower, what[0].toLower)); // let's be creative :-)
				read = Button.new.states_([["Read"]]).action_({
					var sel = ("read" ++ what).asSymbol;
					FileDialog.new({ arg path;
						presetPath = path;
						model.perform(sel, path);
					}, nil, 1, 0, true, presetPath);
				}).maxWidth_(60);
				write = Button.new.states_([["Write"]]).action_({
					var sel = ("write" ++ what).asSymbol;
					FileDialog.new({ arg path;
						presetPath = path;
						model.perform(sel, path);
					}, nil, 0, 1, true, presetPath);
				}).maxWidth_(60);
				HLayout(label, read, write);
			};
			// build program menu
			programMenu = PopUpMenu.new;
			programMenu.action = { model.program_(programMenu.value) };
			programMenu.items_(model.programNames.collect { arg item, index;
				"%: %".format(index, item);
			});
			programMenu.value_(model.program);
			grid.add(HLayout.new(programMenu, open), row, col);
			// try to use another columns if available
			row = (ncolumns > 1).if { 0 } { row + 1 };
			col = (ncolumns > 1).asInt;
			grid.add(makePanel.value("Program"), row, col);
			grid.add(makePanel.value("Bank"), row + 1, col);
			rowOnset = row + 2;
		} {
			grid.add(open, 1, 0);
			programMenu = nil; rowOnset = 2
		};

		// build parameters
		paramSliders = Array.new(nparams);
		paramDisplays = Array.new(nparams);
		nparams.do { arg i;
			var col, row, name, label, display, slider, bar, unit, param;
			param = model.paramCache[i];
			col = i.div(nrows);
			row = i % nrows;
			// param name
			name = StaticText.new
			.string_("%: %".format(i, model.info.parameterNames[i]));
			// param label
			label = StaticText.new.string_(model.info.parameterLabels[i] ?? "");
			// param display
			display = TextField.new
			.fixedWidth_(displayWidth).font_(displayFont).string_(param[1]);
			display.action = {arg s; model.set(i, s.value)};
			paramDisplays.add(display);
			// slider
			slider = Slider.new(bounds: sliderWidth@sliderHeight)
			.fixedSize_(sliderWidth@sliderHeight).value_(param[0]);
			slider.action = {arg s; model.set(i, s.value)};
			paramSliders.add(slider);
			// put together
			bar = View.new.layout_(HLayout.new(name.align_(\left),
				nil, display.align_(\right), label.align_(\right)));
			unit = VLayout.new(bar, slider).spacing_(0);
			grid.add(unit, row+rowOnset, col);
		};
		grid.setRowStretch(rowOnset + nrows, 1);
		grid.setColumnStretch(ncolumns, 1);


		// make the canvas (view) large enough to hold all its contents.
		// somehow it can't figure out the necessary size itself...
		minWidth = ((sliderWidth + 20) * ncolumns).max(sliderWidth);
		minHeight = ((sliderHeight * 4 * nrows) + 120).max(sliderWidth); // empirically
		minSize = minWidth@minHeight;
		view.layout_(grid).fixedSize_(minSize);
	}

	prParam { arg index, value, display;
		paramSliders[index].value_(value);
		paramDisplays[index].string_(display);
	}
	prProgram { arg index, name;
		var items, value;
		programMenu !? {
			value = programMenu.value;
			items = programMenu.items;
			items[index] = "%: %".format(index, name);
			programMenu.items_(items);
			programMenu.value_(value);
		};
	}
	prProgramIndex { arg index;
		programMenu !? { programMenu.value_(index)};
	}
	prFree {
		(this.closeOnFree ?? this.class.closeOnFree).if {
			embedded.not.if {
				view.parent.close;
				^this;
			};
		};
		this.prUpdateGui;
	}
	prOpen {
		model.notNil.if {
			var window, browser, dir, file, editor, search, path, ok, cancel, status, key, absPath;
			var showPath, showSearch, updateBrowser, plugins;
			// prevent opening the dialog multiple times
			dialog !? { ^this };
			// build dialog
			window = Window.new.alwaysOnTop_(true).name_("VST plugin browser");
			browser = ListView.new.selectionMode_(\single);
			browser.action = {
				var info;
				key = plugins[browser.value].asSymbol;
				info = VSTPlugin.plugins(model.synth.server)[key];
				info.notNil.if {
					absPath = info.path;
					showPath.value;
				} { "bug: no info!".error; }; // should never happen
			};
			updateBrowser = {
				var items;
				plugins = VSTPlugin.pluginKeys;
				items = plugins.collect({ arg item;
					var vendor = VSTPlugin.plugins(model.synth.server)[item].vendor;
					// append vendor string
					(vendor.size > 0).if { "% (%)".format(item, vendor) } { item };
				});
				browser.items = items;
				browser.value !? { browser.action.value } ?? { showPath.value };
			};
			updateBrowser.value;

			status = StaticText.new.align_(\left).string_("Path:");
			showPath = { status.stringColor_(Color.black);
				absPath !? { status.string_("Path:" + absPath) } ?? { status.string_("Path:") }
			};
			showSearch = { status.stringColor_(Color.red); status.string_("searching..."); };

			search = Button.new.states_([["Search"]]).maxWidth_(60)
			.toolTip_("Search for VST plugins in the platform specific default paths\n(see VSTPlugin*search)");
			search.action = {
				showSearch.value;
				VSTPlugin.search(model.synth.server, verbose: true, action: {
					{ updateBrowser.value; }.defer;
				});
			};
			dir = Button.new.states_([["Directory"]]).maxWidth_(60)
			.toolTip_("Search a directory for VST plugins");
			dir.action = {
				FileDialog.new({ arg d;
					showSearch.value;
					VSTPlugin.search(model.synth.server, dir: d, useDefault: false, verbose: true, action: {
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
			window.view.addAction({ dialog = nil }, 'onClose');
			dialog = window;
			window.front;
		} { "no model!".error };
	}
	writeName {}
}

