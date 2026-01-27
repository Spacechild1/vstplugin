// TODO: make it work without model, e.g. only for browsing the
// plugins without actually opening any. In that case, we would
// have to omit some UI elements and not make it 'alwaysOnTop'.
// Maybe add VSTPlugin.browse as a shortcut?
//
// We might also add methods for setting/recalling the UI state
// UI state so that VSTPluginController can remember the search
// filters and plugin options. Another possibility would be to
// keep the VSTPluginBrowser instance once it has been opened.
VSTPluginBrowser : Window {
	var model;
	var server;
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
	// data
	var plugins;
	var filteredPlugins;
	var currentPlugin;

	classvar currentDir;

	*new { arg model;
		^super.new.init(model);
	}

	init { arg theModel;
		this.alwaysOnTop = true;
		this.name = "VST plugin browser";

		plugins = [];
		filteredPlugins = [];

		this.prBuildGui;

		model = theModel;
		if (model.notNil) {
			// start at current plugin
			currentPlugin = model.info;
			server = model.synth.server;
		} {
			server = Server.default;
		};

		this.prUpdatePlugins;
	}

	prBuildGui {
		var clearButton, searchButton, searchDirButton;
		var openFileButton, okButton, cancelButton;

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
		editorBox = CheckBox.new(text: "Editor").value_(true);

		multiThreadingBox = CheckBox.new(text: "Multi-threading");

		modeBox = PopUpMenu.new.items_(["normal", "sandbox", "bridge"]);

		// cancel/ok
		cancelButton = Button.new.states_([["Cancel"]])
		.action = { this.close };

		okButton = Button.new.states_([["Open"]])
		.action = {
			if (currentPlugin.notNil) {
				this.prOpenPlugin(currentPlugin.key);
			}
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
			HLayout(StaticText.new.string_("Mode:"), modeBox,
				editorBox, multiThreadingBox, nil, cancelButton, okButton)
		);
	}

	// called when a plugin is selected in the browser
	prPluginSelected { arg index;
		if (index.notNil) {
			currentPlugin = filteredPlugins[index];
			if (currentPlugin.notNil) {
				browserView.toolTip = currentPlugin.prToString;
			};
		} {
			currentPlugin = nil;
			browserView.toolTip = "";
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
			"% (%) %".format(item.key, vendor, bridged); // use key instead of name
		});

		browserView.toolTip_(nil);
		browserView.items = items;

		// restore current plugin
		if (currentPlugin.notNil) {
			filteredPlugins.do { arg item, index;
				(item.key == currentPlugin.key).if { browserView.value_(index) }
			}
		};

		// manually call action to trigger plugin selection
		browserView.action.value;
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
		}, nil, 2, 0, true, currentDir);
	}

	// called when clicking the "File" button
	prOpenFile {
		FileDialog.new({ arg path;
			this.prOpenPlugin(path);
		}, nil, 1, 0, true, currentDir);
	}

	// called when clicking the "Clear" button
	prClear {
		VSTPlugin.clear;
		this.prUpdatePlugins.value;
	}

	prOpenPlugin { arg key;
		var mode = #[\auto, \sandbox, \bridge][modeBox.value];
		var editor = editorBox.value;
		var multiThreading = multiThreadingBox.value;
		model.open(key, editor: editor, multiThreading: multiThreading,
			mode: mode, action: { arg obj, success;
				if (success) {
					currentDir = obj.info.path.dirname;
				};
				{ this.close }.defer;
		});
	}
}
