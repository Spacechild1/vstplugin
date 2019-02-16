VstPluginGui : ObjectGui {
	// class defaults (can be overwritten per instance)
	classvar <>numRows = 10; // max. number of parameters per column
	classvar <>closeOnFree = true;
	classvar <>sliderWidth = 200;
	classvar <>sliderHeight = 20;
	classvar <>displayWidth = 60;
	// private
	var programMenu;
	var paramSliders;
	var paramDisplays;
	var embedded;
	// public
	var <>closeOnFree;
	var <>numRows;
	var <>sliderWidth;
	var <>sliderHeight;
	var <>displayWidth;

	model_ { arg newModel;
		// always notify when changing models (but only if we have a view)
		model.removeDependant(this);
		model = newModel;
		model.addDependant(this);
		view.notNil.if {
			this.update;
		}
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

	prUpdateGui {
		var nparams=0, name, title, ncolumns=0, nrows=0, grid, font, minWidth, minHeight, minSize;
		var numRows = this.numRows ?? this.class.numRows;
		var sliderWidth = this.sliderWidth ?? this.class.sliderWidth;
		var sliderHeight = this.sliderHeight ?? this.class.sliderHeight;
		var displayWidth = this.displayWidth ?? this.class.displayWidth;
		// remove old GUI body
		view !? { view.removeAll };

		model !? { model.info !? {
			name = model.info.name;
			// build program menu
			programMenu = PopUpMenu.new;
			programMenu.action = { model.setProgram(programMenu.value) };
			programMenu.items_(model.programs);
			programMenu.value_(model.currentProgram);
			// parameters: calculate number of rows and columns
			nparams = model.numParameters;
			ncolumns = nparams.div(numRows) + ((nparams % numRows) != 0).asInt;
			(ncolumns == 0).if {ncolumns = 1}; // just to prevent division by zero
			nrows = nparams.div(ncolumns) + ((nparams % ncolumns) != 0).asInt;
		}};

		font = Font.new(*GUI.skin.fontSpecs).size_(14);
		// change window title
		embedded.not.if {
			view.parent.name_(name !? { "VstPlugin (%)".format(name) } ?? { "VstPlugin (empty)" });
		};

		title = StaticText.new(view)
		.stringColor_(GUI.skin.fontColor)
		.font_(font)
		.background_(GUI.skin.background)
		.align_(\center)
		.object_(name ?? "[empty]");

		grid = GridLayout.new;
		grid.add(title, 0, 0);
		programMenu !? { grid.add(programMenu, 1, 0) };

		paramSliders = Array.new(nparams);
		paramDisplays = Array.new(nparams);

		nparams.do { arg i;
			var col, row, name, label, display, slider, bar, unit, param, labelWidth = 50;
			param = model.paramCache[i];
			col = i.div(nrows);
			row = i % nrows;
			// param name
			name = StaticText.new(bounds: sliderWidth@sliderHeight)
			.string_("%: %".format(i, model.info.parameterNames[i]))
			.minWidth_(sliderWidth - displayWidth - labelWidth);
			// param label
			label = StaticText.new(bounds: labelWidth@sliderHeight)
			.string_(model.info.parameterLabels[i] ?? "");
			// param display
			display = TextField.new.fixedWidth_(displayWidth).string_(param[1]);
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
			unit = VLayout.new(bar, slider).spacing_(20);
			grid.add(unit, row+2, col);
		};
		grid.setRowStretch(nrows + 2, 1);

		// add a view and make the area large enough to hold all its contents
		minWidth = ((sliderWidth + 20) * ncolumns).max(sliderWidth);
		minHeight = ((sliderHeight * 3 * nrows) + 70).max(sliderWidth); // empirically
		minSize = minWidth@minHeight;
		view.minSize_(minSize);
		View.new(view).layout_(grid).minSize_(minSize);
	}

	prParam { arg index, value, display;
		paramSliders[index].value_(value);
		paramDisplays[index].string_(display);
	}
	prProgram { arg index, name;
		var items, value;
		value = programMenu.value;
		items = programMenu.items;
		items[index] = name;
		programMenu.items_(items);
		programMenu.value_(value);
	}
	prProgramIndex { arg index;
		programMenu.value_(index);
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
	writeName {}
}

