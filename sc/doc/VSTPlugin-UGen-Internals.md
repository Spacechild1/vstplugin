# VSTPlugin v0.6.0 - UGen documentation

This document is meant for developers who want to interface with the VSTPlugin UGen from other programs or build their own client abstractions.

It describes the UGen structure (i.e. its inputs and outputs), the available OSC messages and the format in which larger data is exchanged between UGen and client(s).

Have a look at `VSTPlugin.sc` and `VSTPluginController.sc` in the *classes* folder for a reference implementation in Sclang.

# UGen Structure

### UGen inputs
| name         | rate  ||
| ------------ | ----- |-|
| flags        | ir    | creation flags (reserved for future use)       |
| blockSize    | ir    | desired block size or 0 (= Server block size)  |
| bypass       | ir/kr | bypass state                                   |
| numInputs    | ir    | number of audio input busses; can be 0         |
| inputBus...  |       | (optional) `<numInputs>` audio input busses    |
| numOutputs   | ir    | number of audio output busses; can be 0        |
| outputBus... |       | (optional) `<numOutputs>` audio output busses  |
| numParamCtls | ir    | number of parameter controls; can be 0         |
| paramCtl...  |       | (optional) `<numParamCtls>` parameter controls |

*inputBus*

The number of channels (`ir`), followed by the actual channel inputs (`ar`).
Example: `2, <ch1>, <ch2>`.

*outputBus*

The number of channels (`ir`).

*paramCtl*

A pair of `<index, value>`.

`index` is the index of the parameter to be automated. Supported rates are `ir` and `kr`. You can dynamically switch between different parameters at control rate. A negative number deactivates the control.

`value` is the new state for the given plugin parameter and should be a floating point number between 0.0 and 1.0. All rates are supported, but `ir` and `kr` are recommended. `ar` only makes sense for VST3 plugins - and only for those which actually support sample accurate automation (many VST3 plugins do not!).

**NOTE**: Automation via UGen inputs can be overriden by the `/map` and `/mapa` Unit commands (see below).

*bypass*

If a plugin is bypassed, processing is suspended and each input is passed straight to its corresponding output. The `bypass` parameter can have the following states:

* 0 -> off (processing)
* 1 -> hard bypass; processing is suspended immediately and the plugin's own bypass method is called (if available). Good plugins will do a short crossfade, others will cause a click. |
* 2 -> soft bypass; if the plugin has a tail (e.g. reverb or delay), it will fade out. Also, this doesn't call the plugin's bypass method, so we can always do a nice crossfade. |


### UGen outputs

| name          | rate ||
| ------------- | ---- |-|
| outputBus...  | ar   | (optional) `<numOutputs>` output busses

Each *outputBus* contains one or more output channels; the number of channels is determined by the corresponding `outputBus` UGen input.

# Realtime safety

VSTPlugin tries its best to be as realtime safe as possible. Plugins are always opened/closed asynchronously on the NRT resp. UI thread. Some Unit commands, like `/reset` or `/program_read`, offer two options via the `async` parameter:

2) `async: true` means that the plugin method is called on the NRT resp. UI thread and will never block the Server! However, plugin processing is temporarily suspended and the UGen will not accept other messages until the command has finished.

1) `async: false` means that the plugin method is simply called on the RT thread. Depending on the plugin, this might be just fine - or block the server. You have to test yourself! The advantage is that the action is performed synchronously and plugin processing doesn't have to be suspended.

**NOTE**: If the plugin is opened with the GUI editor, the `async` option is automatically set to *true* out of thread safety concerns.

# OSC interface

Abbreviations for OSC data types:

| name   | type           |
| ------ | -------------- |
| int    | 32 bit integer |
| float  | 32 bit float   |
| string | string         |
| bytes  | binary blob    |


### Plugin commands

Plugin commands can be called without any UGen instance and take the following form:

`/cmd <string>cmdName (arguments)...`

##### /vst_search

Search for VST plugins.

Arguments:
| type       ||
| ---------- |-|
| int        | a bitset of options (see below)
| int/string | where to write the search results; either a buffer number or a file path; -1 means don't write results.
| float      | timeout (the time to wait for each plugin before it is regarded as stuck and ignored); 0.0 means no timeout.
| int        | the number of user supplied search paths; 0 means none.
| string...  | (optional) list of user supplied search paths
| int        | the number of exclude paths; 0 means none
| string...  | (optional) list of user supplied exclude paths
| string     | (optional) custom cache file directory

This will search the given paths recursively for VST plugins, probe them, and write the results to a file or buffer. Valid plugins are stored in a server-side plugin dictionary. If no plugin could be found, the buffer or file will be empty.

The following options can be combined with a bitwise OR operation:
| value ||
| ----- |-|
| 0x1   | verbose (print plugin paths and probe results)
| 0x2   | write search results to cache file.
| 0x4   | probe in parallel (faster, but might cause audio dropouts because of full CPU utilization)

If there are no user supplied search paths, the standard VST search paths are used instead:

- VST 2.x
  - Windows
    - `%ProgramFiles%`\VSTPlugins
    - `%ProgramFiles%`\Steinberg\VSTPlugins
    - `%ProgramFiles%`\Common Files\VST2
    - `%ProgramFiles%`\Common Files\Steinberg\VST2
  - macOS
    - ~/Library/Audio/Plug-Ins/VST
    - /Library/Audio/Plug-Ins/VST
  - Linux
    - ~/.vst
    - /usr/local/lib/vst
    - /usr/lib/vst
- VST 3.x
  - Windows
    - `%ProgramFiles%`\Common Files\VST3
  - macOS
    - ~/Library/Audio/Plug-Ins/VST3
    - /Library/Audio/Plug-Ins/VST3
  - Linux
    - ~/.vst3
    - /usr/local/lib/vst3
    - /usr/lib/vst3

**NOTE**: Here, `%ProgramFiles%` stands for "C:\Program Files" on a 64 bit Server and "C:\Program Files (x86)" on a 32 bit Server.

##### /vst_search_stop

Stop a running search. (No arguments)

##### /vst_query

Try to obtain a plugin description from the plugin cache.

Arguments:
| type       ||
| ---------- |-|
| string     | plugin path/key (absolute or relative)
| int/string | where to write the result; either a buffer number or a file path. -1 means don't write results.

If the plugin is not contained in the plugin cache, it is searched in the standard VST search paths and probed in a seperate process (so that bad plugins don't crash the server).
On success, the plugin information is written to a file or buffer and the plugin is stored in a server-side plugin dictionary; on fail, nothing is written.

For VST 2.x plugins, you can omit the file extension. Relative paths are resolved recursively based on the standard VST directories.

The probe process can return one of the following results:

- ok -> probe succeeded
- failed -> couldn't load the plugin; possible reasons:
    a) not a VST plugin, b) wrong architecture, c) missing dependencies
- crashed -> the plugin crashed on initialization
- error -> internal failure

##### /vst_cache_read

Read the cache file from a custom location.

Arguments:
| type   ||
| ------ |-|
| string | the cache file directory |

##### /vst_clear

Clear the server-side plugin dictionary.

Arguments:
| type ||
| ---- |-|
| int  | remove cache file; 1 = yes, 0 = no |


##### /dsp_threads

Set the number of DSP threads for multi-threaded plugin processing.
(By default, this is the number of logical CPUs.)

**NOTE**: This command only takes effect if it is sent before any (multi-threaded) plugins have been opened.

Arguments:
| type   ||
| ------ |-|
| int    | number of threads; 0 = default |


### Plugin key

Plugins are stored in a server-side plugin dictionary under its *key*.
For VST 2.x plugins, the key is simply the plugin name; for VST 3.x plugins, the key is the plugin name plus ".vst3" extension.

If a VST2 and VST3 module contains a single plugin (as is the case with most VST2 and VST3 plugins), it can also be referenced by its file path. Since VST2 shell plugins and VST3 plugin factories contain multiple plugins in a single module, each plugin can only be referenced by its *key*.

On big advantage of plugin keys over file paths is that the latter can vary across systems and architectures, whereas the former is always the same!

### Unit commands

Unit commands are used to control a specific UGen instance and take the following form:

`/u_cmd <int>nodeID <int>synthIndex <string>cmdName (arguments)...`

##### /open

Open a new VST plugin. This method is always asynchronous.

Arguments:
| type   ||
| ------ |-|
| string | plugin key or path.
| int    | request VST GUI editor; 1 = yes, 0 = no
| int    | request multithreading; 1 = yes, 0 = no
| int    | mode; 0 = normal, 1 = sandbox (dedicated process), 2 = bridge (shared process)

Replies with
| `/vst_open` ||
| --------    |-|
| int         | node ID
| int         | synth index
| float       | 1.0 = success, 0.0 = fail
| float       | VST GUI editor; 1.0 = yes, 0.0 = no
| float       | initial latency in samples

##### /close

Close the current VST plugin.

No Arguments.

##### /vis

Show/hide the VST GUI editor (if enabled).

Arguments:
| type ||
| ---- |-|
| int  | 1 = show, 0 = hide

##### /pos

Set the position of the GUI editor.

Arguments:
| type ||
| ---- |-|
| int  | x-coordinate |
| int  | y-coordinate |

##### /vis

Resize the GUI editor.
This only works for VST3 plugins with a resizable GUI.

Arguments:
| type ||
| ---- |-|
| int  | width  |
| int  | height |

##### /reset

Reset the VST plugin's internal state (e.g. clear delay lines).

Arguments:
| type ||
| ---- |-|
| int  | asynchronous; 1 = true, 0 = false

**NOTE**: If the plugin is opened with the VST GUI editor, the command is always performed asynchronously and the `async` argument is ignored.

### Parameters

##### /set

Set a plugin parameter to a new value.

Arguments:
| type         ||
| ------------ |-|
| int/string   | parameter index or name
| float/string | "normalized" float value between 0.0 and 0.1 or "plain" string representation. Depending on the plugin, the latter might only work for certain parameters - or not work at all.
| ...          | (optional) additional pairs of `<index>, <value>` 

Replies with `/vst_param` (see "Events" section).

You can automate several parameters at once, e.g. `/u_cmd, <nodeID>, <synthIndex>, /set, 0, 0.5, 1, "12", "Wet", 0.7` will set parameters 0, 1 and "Wet".

**NOTE**: `/set` will automatically unmap the corresponding parameter(s) from any audio/control busses, see `/unmap`.

##### /setn

Like `/set`, but with a range of subsequent parameters.

Arguments:
| type             ||
| ---------------- |-|
| int/string       | name or index of start parameter
| int              | number of parameters
| float/string ... | parameter values

##### /get

Get a single plugin parameter value.

Arguments:
| type       ||
| ---------- |-|
| int/string | parameter index or name

Replies with:
| `/vst_set`||
| --------- |-|
| int       | node ID
| int       | synth index
| float     | parameter index
| float     | normalized parameter value

##### /getn

Get a range of plugin parameter values.

Arguments:
| type       ||
| ---------- |-|
| int/string | parameter index or name
| int        | number of subsequent parameters (-1: till the end)

Replies with:
| `/vst_set`||
| --------- |-|
| int       | node ID
| int       | synth index
| float     | parameter start index
| float     | number of parameters
| float ... | normalized parameter values

To get all parameter values, you can do `/u_cmd, <nodeID>, <synthIndex>, /getn, 0, -1`.

##### /param_query

Query parameter states.

Arguments:
| type       ||
| ---------- |-|
| int        | start parameter index
| int        | number of parameters

Replies with a series of `/vst_param` messages (see "Events" section). Use this instead of `/getn`, if you also need the string representation, e.g. to update your client GUI after loading a new plugin, switching programs or loading a preset file.

##### /map

Map a subsequent range of parameters to control bus channels.

Arguments:
| type       ||
| ---------- |-|
| int/string | name or index of parameter
| int        | control bus start index
| int        | number of channels

For example, `/u_cmd, <nodeID>, <synthIndex>, /map, 5, 4, 1` will map parameter 5 to control bus 4. `/u_cmd, <nodeID>, <synthIndex>, /map, 5, 4, 2` will map parameter 5 to control bus 4 and also parameter 6 to control bus 5.


##### /mapa

Map a subsequent range of parameters to audio bus channels.

Arguments:
| type       ||
| ---------- |-|
| int/string | name or index of parameter
| int        | audio bus start index
| int        | number of channels

**NOTE**: Mapping a parameter to a audio/control bus has higher precedence than UGen input parameter controls (see "UGen inputs" section).

##### /unmap

Unmap parameters from audio/control busses.

Arguments:
| type           ||
| -------------- |-|
| int/string ... | names or indices of parameters to unmap. If omitted, all parameters are unmapped.

**NOTE**: `/set` will automatically unmap the corresponding parameter(s) from any audio/control busses!

### Preset management

##### /program_set

Select program from program list.

Arguments:
| type ||
| ---- |-|
| int  | program index |

##### /program_name

Change the current program name.

Arguments:
| type   ||
| ------ |-|
| string | new program name

Replies with `/vst_program`, see "Events" section.

##### /program_read

Read a VST program from a file or sound buffer.

Arguments:
| type       ||
| ---------- |-|
| int/string | buffer number of file path, see "Data transfer" section.
| int        | asynchronous; 1 = yes, 0 = no

Replies with:
| `/vst_program_read` ||
| ------------------- |-|
| int                 | node ID
| int                 | synth index
| float               | 1.0 = success, 0.0 = fail

Also sends `/vst_program` (see "Event" section).

##### /bank_read

Read a preset bank from a file or buffer.

Arguments:
| type       ||
| ---------- |-|
| int/string | buffer number of file path, see "Data transfer" section.
| int        | asynchronous; 1 = yes, 0 = no

Replies with:
| `/vst_bank_read` ||
| ---------------- |-|
| int              | node ID
| int              | synth index
| float            | 1.0 = success, 0.0 = fail

Also sends `/vst_program` and `/vst_program_index` (see "Events" section).

##### /program_write

Write a preset program to a file or buffer.

Arguments:
| type       ||
| ---------- |-|
| int/string | buffer number of file path, see "Data transfer" section.
| int        | asynchronous; 1 = yes, 0 = no

Replies with:
| `/vst_program_write` ||
| -------------------- |-|
| int                  | node ID
| int                  | synth index
| float                | 1.0 = success, 0.0 = fail

##### /bank_write

Write a preset bank to a file or buffer.

Arguments:
| type       ||
| ---------- |-|
| int/string | buffer number of file path, see "Data transfer" section.
| int        | asynchronous; 1 = yes, 0 = no

Replies with:
| `/vst_bank_write` ||
| ----------------- |-|
| int               | node ID
| int               | synth index
| float             | 1.0 = success, 0.0 = fail

##### /program_query

Query program names.

Arguments:
| type ||
| ---- |-|
| int  | start program index
| int  | number of programs

Replies with a series of `/vst_program` messages (see "Event" section). For example, you can use this to update the program list in your client after loading a new plugin or reading a preset bank.

### MIDI

##### /midi_msg

Send a MIDI message.

Arguments:
| type  ||
| ----- |-|
| bytes | MIDI message (1-3) bytes
| float | (optional) detune in cent

The optional detune argument allows to detune a single note-on or note-off message; this feature is not part of the MIDI standard and only a few VST plugins suppport it.

##### /midi_sysex

Send a SysEx message.

| type  ||
| ----- |-|
| bytes | SysEx message

### Timing and transport

##### /tempo

Set the tempo.

Arguments:
| type  ||
| ----- |-|
| float | BPM (beats per minute)

##### /time_sig

Setthe time signature.

Arguments:
| type ||
| ---- |-|
| int  | numerator
| int  | denominator

##### /transport_play

Set the transport state.

Arguments:
| type ||
| ---- |-|
| int  | 1 = play, 0 = stop

##### /transport_set

Set the transport position.

Arguments:
| type  ||
| ----- |-|
| float | transport position in beats.

##### /transport_get

Get the current transport position.

Replies with:
| `/vst_transport` ||
| ---------------- |-|
| int              | node ID
| int              | synth index
| float            | position in beats

### VST 2.x

**NOTE**: The following methods only work for VST 2.x plugins and are meant for expert users. Please check the VST 2.x SDK for more information.

##### /can_do

Ask the plugin if it can do something.

Arguments:
| type   ||
| ------ |-|
| string | what

Replies with:
| `/vst_can_do` ||
| ------------- |-|
| int           | node ID
| int           | synth index
| float         | 1.0 = yes, -1.0 = no, 0.0 = don't know

##### /vendor_method

Call a vendor specific method.

Arguments:
| type   ||
| ------ |-|
| int    | index
| int    | value
| int    | ptr
| int    | opt
| int    | asynchronous; 1 = yes, 0 = no

Replies with:
| `/vst_vendor_method` ||
| -------------------- |-|
| int                  | node ID
| int                  | synth index
| float                | result


### Events

The VSTPlugin UGen may send the following events as Node reply messages:

##### /vst_param

A parameter has changed state.

Arguments:
| type      ||
| --------- |-|
| int       | node ID
| int       | synth index
| float     | parameter index
| float     | normalized parameter value
| float ... | "plain" string representation, see "String encoding" section.

`/vst_param` messages are sent after `/set`, `/setn`, `/param_query` or when automating parameters in the VST GUI editor. They are *not* sent when parameters are automated via UGen input parameter controls (see "UGen inputs" section) or audio/control bus mappings (see `/map`), because this would lead to excessive OSC traffic.

**NOTE**: A single parameter change might produce several `/vst_param` messages, e.g. in the case of linked parameters.

##### /vst_program

A program name has changed.

Arguments:
| type      ||
| --------- |-|
| int       | node ID
| int       | synth index
| float     | program index
| float ... | program name, see "String encoding" section.

This message is sent after `/program_name`, `/program_read` or `/bank_read`.

##### /vst_program_index

The current program index has changed.

Arguments:
| type      ||
| --------- |-|
| int       | node ID
| int       | synth index
| float     | program index

This message is sent after `/program_set` and `/bank_read`.

##### /vst_auto

A parameter has been automated in the VST GUI editor.

Arguments:
| type      ||
| --------- |-|
| int       | node ID
| int       | synth index
| float     | parameter index
| float     | normalized parameter value

This message is always sent together with `/vst_param`.

##### /vst_latency

The plugin's processing latency has changed. This includes the additional latency caused by multithreading (see `/open`).

Arguments:
| type      ||
| --------- |-|
| int       | node ID
| int       | synth index
| float     | latency in samples

##### /vst_midi

The plugin has sent a MIDI message.

Arguments:
| type       ||
| ---------- |-|
| int        | node ID
| int        | synth index
| float  ... | MIDI message (one float per byte)

### /vst_sysex

The plugin has sent a SysEx message.

Arguments:
| type       ||
| ---------- |-|
| int        | node ID
| int        | synth index
| float  ... | SysEx message (one float per byte)

### /vst_update

Multiple parameters have changed internally,
e.g. because the user loaded a preset in the plugin UI.

Arguments:
| type       ||
| ---------- |-|
| int        | node ID
| int        | synth index

### /vst_crash

The plugin has crashed.
Naturally, this message can only be sent if the plugin has been bridged or sandboxed.

Arguments:
| type       ||
| ---------- |-|
| int        | node ID
| int        | synth index


# String encoding

Because of limitations in the SuperCollider plugin API, Node reply messages can only contain float arguments. Our solution is to encode string arguments as a list of bytes with a length prefix ("Pascal strings"), where each byte takes up a single float.

For example, the string "Dry" is encoded as `3.0, 44.0, 72.0, 79.0`.


# Data transfer

To reliably exchange larger data sets between server and clients, you can use temp files or sound buffers. For locals servers, temp files should be preferred. For remote servers, you have to use sound buffers (unless you can use FTP :-). Usually, client frameworks already have abstractions to (more or less) reliably stream data from/to sound buffers.

Files are always read and written in binary. In case of sound buffers, each float corresponds to a single byte.

Whenever VSTPlugin writes data to a sound buffer, it will allocate the data for you, but because of limitations in the SuperCollider plugin API, it can't safely deallocate a sound buffer. There are two consequences:

- Whenever you ask VSTPlugin to *write* data, make sure that the target sound buffer is empty!
- Whenever you ask VSTPlugin to *read* data, it's your job to free the sound buffer after the command has finished.

VSTPlugin uses a custom format similar to .ini files to exchange plugin description data between server and clients. It is also used in the plugin cache file.

### Plugin info

This is the structure of a single plugin description, as used by `/vst_query`:

```
[plugin]
id=<unique ID string>
path=<file path>
name=<plugin name>
vendor=<vendor name>
category=<category name>
version=<plugin version string>
sdkversion=<VST SDK version string>
pgmchange=<program change parameter index in hex> // optional
bypass=<bypass parameter index in hex> // optional
flags=<bitset>
[inputs]
n=<input bus count>
<channel count #0>, <type #0>, <name #0>
<channel count #1>, <type #1>, <name #1>
...
<channel count #N-1>, <type #N-1>, <name #N-1>
[outputs]
n=<output bus count>
<channel count #0>, <type #0>, <name #0>
<channel count #1>, <type #1>, <name #1>
...
<channel count #N-1>, <type #N-1>, <name #N-1>
[parameters]
n=<parameter count>
<name #0>, <label #0>, <ID #0>, <flags #0>
<name #1>, <label #1>, <ID #1> <flags #1>
...
<name #N-1>, <label #N-1>, <ID #N-1>, <flags #N-1>
[programs]
n=<program count>
<name #0>
<name #1>
...
<name #N-1>
[keys]
n=<number of keys>
<key #0>
<key #1>
...
<key #N-1>
```

String values, like plugin/parameter/program names, can contain any characters except newlines and commas (those are bashed to a replacement symbol by the UGen).

##### flags

`flags` is a bitset of boolean properties, written as a hexidecimal number. The following flags can be combined with a bitwise OR operation:

| value ||
| ----- |-|
| 0x001 | supports the GUI editor
| 0x002 | is a VST instrument
| 0x004 | supports single precision processing
| 0x008 | supports double precision processing
| 0x010 | has MIDI input
| 0x020 | has MIDI output
| 0x040 | has SysEx input
| 0x080 | has SysEx output
| 0x100 | is bridged

##### input/output busses

Each bus entry takes up a single line and consists of three fields, separated by a comma: `<channel count>, <type>, <name>`.

`<type>` can be either 0 (= main) or 1 (= aux). `<name` is always empty for VST2 plugins (because they only have a single input/output bus).

##### parameters

Each parameter entry takes up a single line and consists of three fields, separated by a comma: `<name>, <label>, <ID> <flags>`.

`<label>` is the unit of measurement (e.g. "dB", "ms", "%"); it can be an empty string!

The parameter ID is a hexidecimal number. For VST 2.x plugins it is the same as the parameter index, but for VST 3.x plugins it can be an arbitrary 32 bit integer.

`flags` is a bitset of boolean properties, written as a hexidecimal number. The following flags can be combined with a bitwise OR operation:

| value ||
| ----- |-|
| 0x01  | is automatable |

##### programs

**NOTE**: Program names can be empty strings; this means that empty lines after the `[programs]` section are significant and must not be ignored!


Each plugin can be referred to by one or more *keys*. The primary key always comes first in the list.

*EXAMPLE:*
```
[plugin]
path=C:/Program Files/VSTPlugins/GVST/GChorus.dll
id=6779416F
name=GChorus
vendor=GVST
category=Effect
version=1200
sdkversion=VST 2.4
flags=10d
[inputs]
n=1
2,0,
[outputs]
n=1
2,0,
[parameters]
n=4
Depth,cents,0
Freq,Hz,1
R Phase,deg,2
Mix,%,3
[programs]
n=10
Subtle Insert
Wide Insert
Heavy Insert
Subtle Send
Wide Send
Heavy Send
Defaults
Defaults
Defaults
Defaults
[keys]
n=2
GChorus
C:/Program Files/VSTPlugins/GVST/GChorus.dll
```

### Search results

This is used by `/vst_search` to transmit search results to the client. It has the following structure:

```
[plugins]
n=<number of plugins>
<plugin info #0>
<plugin info #1>
...
<plugin info #N-1>
```

Each `<plugin info>` entry has the same structure as in "Plugin info".


### Plugin cache file

Probing lots of (large) VST plugins can be a slow process.
To speed up subsequent searches, the search results can be written to a cache file (see `/vst_search`), which is located in a platform specific directory:
`%LOCALAPPDATA%\vstplugin\sc` on Windows, `~/Library/Application Support/vstplugin/sc` on macOS and `$XDG_DATA_HOME/vstplugin/sc` resp. `~/.local/share/vstplugin/sc` on Linux.
The cache file itself is named `cache_<arch>.ini`, so that cache files for different CPU architectures can co-exist.

The cache file structure is very similar to that in "Search results".
The only difference is that it also contains a version header (`[version]`) and a plugin black-list (`[ignore]`).

```
[version]
<major>.<minor>.<patch>
[ignore]
n=<number of paths>
<path #0>
<path #1>
...
<path #N-1>
[plugins]
n=<number of plugins>
<plugin info #0>
<plugin info #1>
...
<plugin info #N-1>
```

Plugins are black-listed if the probe process failed (see `/vst_query`).
If you want to replace a "bad" plugin with a "good" one, you have to first remove the cache file (see `/vst_clear`).

