// TODO: add methods for setting/recalling the window state
// and filter/plugin options.
VSTPluginBrowser : Window {
	var <>server;
	var <>action;

	// data
	var <currentPlugin;
	var autoClose;
	var plugins;
	var filteredPlugins;

	// widgets
	var browserView;
	var statusLabel;
	var editorBox;
	var multiThreadingBox;
	var modeBox;
	var stringFilter;
	var vendorFilter;
	var categoryFilter;
	var vst2Filter;
	var vst3Filter;
	var fxFilter;
	var synthFilter;
	var showBridged;
	var okButton;

	classvar currentDir;

	*new { arg mode=\normal, server;
		^super.new.initBrowser(mode, server);
	}

	initBrowser { arg mode, theServer;
		if (#[ \normal, \dialog, \browse ].includes(mode).not) {
			^Error("bad value for 'mode' argument (%)".format(mode)).throw;
		};

		this.name = "VST plugin browser";
		// in dialog mode, the window should stay on top to emulate
		// a modal dialog window. we also automatically close the
		// window when the user clicks the "Open" button.
		if (mode == \dialog) {
			this.alwaysOnTop = true;
			autoClose = true;
		} {
			autoClose = false;
		};

		plugins = [];
		filteredPlugins = [];
		server = theServer ?? { Server.default };

		this.prBuildGui(mode);
		this.prUpdatePlugins;
	}

	deleteOnClose { ^view.deleteOnClose }
	deleteOnClose_ { arg boolean; view.deleteOnClose_(boolean) }

	destroy { view.destroy }

	currentPlugin_ { arg info;
		currentPlugin = info;
		// try to select in browser
		if (info.notNil) {
			filteredPlugins.do { arg item, index;
				if (item.key == info.key) {
					browserView.valueAction_(index);
					^this;
				}
			}
		};
		// otherwise deselect
		browserView.valueAction_(nil);
		// NB: valueAction_ will trigger prPluginSelected
	}

	prBuildGui { arg mode;
		var clearButton, searchButton, searchDirButton;
		var openFileButton, cancelButton;

		// the plugin list
		browserView = ListView.new.selectionMode_(\single);
		// called when a plugin is selected
		browserView.action = { this.prPluginSelected(browserView.value) };

		// plugin filters
		// update on every key input. the delay makes sure we really
		// see the updated text!
		stringFilter = TextField.new.minWidth_(60)
		.addAction({ AppClock.sched(0, { this.prApplyFilter }) }, 'keyDownAction');

		vst2Filter = CheckBox.new(text: "VST2")
		.value_(true).action_({ this.prApplyFilter });

		vst3Filter = CheckBox.new(text: "VST3")
		.value_(true).action_({ this.prApplyFilter });

		fxFilter = CheckBox.new(text: "FX")
		.value_(true).action_({ this.prApplyFilter });

		synthFilter = CheckBox.new(text: "Instrument")
		.value_(true).action_({ this.prApplyFilter });

		showBridged = CheckBox.new(text: "Show bridged plugins")
		.value_(true).action_({ this.prApplyFilter });

		vendorFilter = PopUpMenu.new.items_(["All"])
		.action_({ this.prApplyFilter });

		categoryFilter = PopUpMenu.new.items_(["All"])
		.action_({ this.prApplyFilter });

		// status bar
		statusLabel = StaticText.new.align_(\left);

		// search buttons
		searchButton = Button.new.states_([["Search"]])
		.toolTip_("Search for VST plugins in the platform specific default paths\n(see VSTPlugin*search)")
		.action_({ this.prSearch });

		searchDirButton = Button.new.states_([["Directory"]])
		.toolTip_("Search a directory for VST plugins")
		.action_({ this.prSearchDir });

		openFileButton = Button.new.states_([["File"]])
		.toolTip_("Open a VST plugin file")
		.action_({ this.prOpenFile });

		clearButton = Button.new.states_([["Clear"]])
		.toolTip_("Clear the plugin cache")
		.action_({ this.prClear });

		// plugin options
		if (mode != \browse) {
			editorBox = CheckBox.new(text: "Editor").value_(true);

			multiThreadingBox = CheckBox.new(text: "Multi-threading");

			modeBox = PopUpMenu.new.items_(["normal", "sandbox", "bridge"]);
		};

		// cancel/ok buttons
		if (mode == \dialog) {
			cancelButton = Button.new.states_([["Cancel"]])
			.action_({
				action.value(nil); // signify cancellation
				this.close
			});
		};

		if (mode != \browse) {
			okButton = Button.new.states_([["Open"]])
			.action_({ this.prOpen });
		};

		this.layout = VLayout(
			browserView,
			HLayout(
				[StaticText.new.string_("Find:"), stretch: 0],
				[stringFilter, stretch: 1],
				[StaticText.new.string_("Vendor:"), stretch: 0],
				[vendorFilter, stretch: 1],
				[StaticText.new.string_("Category:"), stretch: 0],
				[categoryFilter, stretch: 1]
			),
			HLayout(
				vst2Filter, vst3Filter, fxFilter,
				synthFilter, nil, showBridged
			),
			HLayout(
				searchButton, searchDirButton, openFileButton,
				clearButton, nil, statusLabel
			),
			8,
			if (mode != \browse) {
				HLayout(StaticText.new.string_("Mode:"), modeBox,
					editorBox, multiThreadingBox, nil, cancelButton, okButton
				)
			} { HLayout(nil, cancelButton, okButton) }
		);
	}

	// called when a plugin is selected in the browser
	prPluginSelected { arg index;
		if (index.notNil) {
			// do not use this.currentPlugin!
			currentPlugin = filteredPlugins[index];
			if (currentPlugin.notNil) {
				browserView.toolTip = currentPlugin.prToString;
			};
		} {
			currentPlugin = nil;
			browserView.toolTip_(nil);
		};
		if (okButton.notNil) {
			// only enable OK button if a plugin is selected
			okButton.enabled_(currentPlugin.notNil);
		}
	}

	// called when one of the filters change
	prApplyFilter {
		var items;

		filteredPlugins = plugins.select({ arg item;
			var ok = true, vst3 = item.sdkVersion.find("VST 3").notNil;
			var phrase = stringFilter.string.toLower;

			if (phrase.size > 0) {
				// search plugin and vendor name
				ok = item.name.toLower.find(phrase).notNil or: { item.vendor.toLower.find(phrase).notNil };
			};

			// use shortcircuiting to skip test if 'ok' is already 'false'
			ok = ok and: {
				(vst2Filter.value && vst3.not) or:
				{ vst3Filter.value && vst3 }
			};

			ok = ok and: {
				(fxFilter.value && item.synth.not) or:
				{ synthFilter.value && item.synth }
			};

			ok = ok and: {
				item.bridged.not || showBridged.value
			};

			if (vendorFilter.value > 0) {
				ok = ok and: {
					(item.vendor.size > 0).if {
						item.vendor == vendorFilter.item;
					} {
						vendorFilter.item == "[unknown]";
					}
				};
			};

			if (categoryFilter.value > 0) {
				ok = ok and: {
					item.category.split($|).indexOfEqual(categoryFilter.item).notNil;
				}
			};

			ok;
		});

		items = filteredPlugins.collect({ arg item;
			var vendor = if (item.vendor.size > 0) { item.vendor } { "unknown" };
			var bridged = if (item.bridged) { "[bridged]" } { "" };
			// show key instead of anme
			"% (%) %".format(item.key, vendor, bridged);
		});

		browserView.toolTip_(nil);
		browserView.items = items;
		// restore current plugin
		this.currentPlugin = currentPlugin;
	}

	// called after a new search
	prUpdatePlugins {
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
		if (oldCategory.notNil) {
			categoryFilter.items.do { arg item, index;
				(item == oldCategory).if { categoryFilter.value_(index) }
			}
		};
		if (oldVendor.notNil) {
			vendorFilter.items.do { arg item, index;
				(item == oldVendor).if { vendorFilter.value_(index) }
			}
		};

		// now filter the plugins
		this.prApplyFilter;

		this.prSearchRunning(false);
	}

	// show that the search is running
	prSearchRunning { arg running;
		if (running) {
			statusLabel.stringColor_(Color.red).string_("searching...");
		} { statusLabel.string_("") };
	}

	// called when clicking the "Search" button
	prSearch {
		this.prSearchRunning(true);
		VSTPlugin.search(server, verbose: true, action: {
			{ this.prUpdatePlugins; }.defer;
		});
	}

	// called when clicking the "Directory" button
	prSearchDir {
		FileDialog.new({ arg dir;
			this.prSearchRunning(true);
			VSTPlugin.search(server, dir: dir, verbose: true, action: {
				{ this.prUpdatePlugins; }.defer;
			});
			currentDir = dir;
		}, nil, 2, 0, true, currentDir);
	}

	// called when clicking the "File" button
	prOpenFile {
		FileDialog.new({ arg path;
			currentDir = path.dirname;
			VSTPlugin.prQuery(server, path, action: { |info|
				if (info.notNil) {
					{
						currentPlugin = info;
						this.prUpdatePlugins;
					}.defer;
				}
			});
		}, nil, 1, 0, true, currentDir);
	}

	// called when clicking the "Clear" button
	prClear {
		VSTPlugin.clear;
		this.prUpdatePlugins.value;
	}

	// called when clicking the "Open" button
	prOpen {
		var options;
		if (currentPlugin.notNil) {
			// NB: prOpen is never called in \browse mode because
			// there is no "Open" button.
			options = ();
			options[\mode] = #[\auto, \sandbox, \bridge][modeBox.value];
			options[\editor] = editorBox.value;
			options[\multiThreading] = multiThreadingBox.value;

			this.action.value(currentPlugin, options);

			if (autoClose) {
				this.close;
			}
		} {
			// shouldn't really happen because the "Open" button
			// should be disabled when currentPlugin is nil.
			"'Open' clicked without plugin! This is a bug!".error;
		}
	}
}
