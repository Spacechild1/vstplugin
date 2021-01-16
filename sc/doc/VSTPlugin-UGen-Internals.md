# VSTPlugin - UGen documentation

This document is meant for developers who want to interface with the VSTPlugin UGen from other programs or build their own client abstractions.

It describes the UGen structure (i.e. its inputs and outputs), the available OSC messages and the format in which larger data is exchanged between UGen and client(s).

Have a look at `VSTPlugin.sc` and `VSTPluginController.sc` in the *classes* folder for a reference implementation in Sclang.

# UGen Structure

### UGen inputs
| name         | rate  ||
| ------------ | ----- ||
| numOutputs   | ir    | number of main audio outputs                       |
| flags        | ir    | creation flags (reserved for future use)           |
| bypass       | ir/kr | bypass state                                       |
| numInputs    | ir    | number of main audio inputs; can be 0              |
| inputs...    | ar    | (optional) `<numInputs>` main audio inputs         |
| numAuxInputs | ir    | number of auxiliary audio inputs; can be 0         |
| auxInputs... | ar    | (optional) `<numAuxInputs>` auxiliary audio inputs |
| numParams    | ir    | number of parameter controls; can be 0             |
| params...    | *     | (optional) `2 * <numParams>` parameter controls    |

If a plugin is bypassed, processing is suspended and each input is passed straight to its corresponding output. The `bypass` parameter can have the following states:

* 0 -> off (processing)
* 1 -> hard bypass; processing is suspended immediately and the plugin's own bypass method is called (if available). Good plugins will do a short crossfade, others will cause a click. |
* 2 -> soft bypass; if the plugin has a tail (e.g. reverb or delay), it will fade out. Also, this doesn't call the plugin's bypass method, so we can always do a nice crossfade. |

Each parameter control is a pair of `<index, value>`, so `params` takes up 2 * `numParams` UGen inputs in total.

`index` is the index of the parameter to be automated. Supported rates are `ir` and `kr`. You can dynamically switch between different parameters at control rate. A negative number deactivates the control.

`value` is the new state for the given plugin parameter and should be a floating point number between 0.0 and 1.0. All rates are supported, but `ir` and `kr` are recommended. `ar` only makes sense for VST3 plugins - and only for those which actually perform sample accurate automation (many VST3 plugins do not!).

**NOTE**: Automation via UGen inputs can be overriden by the `/map` and `/mapa` Unit commands (see below).

### UGen outputs

| name          | rate ||
| ------------- | ---- ||
| outputs...    | ar   | (optional) <numOutputs> main audio outputs
| auxOutputs... | ar   | (optional) auxiliary audio outputs (`UGen outputs - <numOutputs>`)


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
| int/string | where to write the search results; either a buffer number or a file path. -1 means don't write results.
| string...  | (optional) user supplied search paths

This will search the given paths recursively for VST plugins, probe them, and write the results to a file or buffer. Valid plugins are stored in a server-side plugin dictionary. If no plugin could be found, the buffer or file will be empty.

The following options can be combined with a bitwise OR operation:
| value ||
| ----- |-|
| 0x1   | use standard VST paths, see below.
| 0x2   | verbose (print plugin paths and probe results)
| 0x4   | add search results to cache file.
| 0x8   | probe in parallel (faster, but might cause audio dropouts because of full CPU utilization)

The standard VST search paths are:

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

##### /vst_probe

Probe a VST plugin.

Arguments:
| type       ||
| ---------- |-|
| string     | plugin path (absolute or relative)
| int/string | where to write the search results; either a buffer number or a file path. -1 means don't write results.

This will probe a given plugin file in a seperate process, so that bad plugins don't crash the server. On success, the plugin information is written to a file or buffer and the plugin is stored in a server-side plugin dictionary; on fail, nothing is written. If you don't need the result (e.g. in NRT synthesis), you can pass a negative buffer number.

For VST 2.x plugins, you can omit the file extension. Relative paths are resolved recursively based on the standard VST directories.

The probe process can return one of the following results:

- ok -> probe succeeded
- failed -> couldn't load the plugin; possible reasons:
    a) not a VST plugin, b) wrong architecture, c) missing dependencies
- crashed -> the plugin crashed on initialization
- error -> internal failure

##### /vst_clear

Clear the server-side plugin dictionary.

Arguments:
| type ||
| ---- |-|
| int  | remove cache file; 1 = yes, 0 = no |


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

##### /parameter_query

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

Replies with `/vst_program_index`, see "Events" section.

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

This is the structure of a single plugin description, as used by `/vst_probe`:

```
[plugin]\n
id=<unique ID string>\n
path=<file path>\n
name=<plugin name>\n
vendor=<vendor name>\n
category=<category name>\n
version=<plugin version string>\n
sdkversion=<VST SDK version string>\n
inputs=<max. number of audio inputs>\n
auxinputs=<max. number of auxiliary inputs>\n // optional
outputs=<max. number of audio outputs>\n
auxoutputs=<max. number of auxiliary inputs>\n // optional
pgmchange=<program change parameter index in hex>\n // optional
bypass=<bypass parameter index in hex>\n // optional
flags=<bitset>\n
[parameters]\n
n=<number of parameters>\n
<parameter name #0>, <parameter label #0>, <parameter ID #0>\n
<parameter name #1>, <parameter label #1>, <parameter ID #1>\n
...
<parameter name #N-1>, <parameter label #N-1>, <parameter ID #N-1>\n
[programs]\n
n=<number of programs>\n
<program name #0>\n
<program name #1>\n
...
<program name #N-1>\n
[keys]\n
n=<number of keys>\n
<key #0>\n
<key #1>\n
...
<key #N-1>\n
```

`flags` is a bitset of boolean properties, written as a hexidecimal number. The following flags can be combined with a bitwise OR operation:
| value ||
| ----- |-|
| 0x1   | supports the GUI editor
| 0x2   | is a VST instrument
| 0x4   | supports single precision processing
| 0x8   | supports double precision processing
| 0x10  | has MIDI input
| 0x20  | has MIDI output
| 0x40  | has SysEx input
| 0x80  | has SysEx output

String values, like plugin/parameter/program names, can contain any characters except newlines and commas. (Those are bashed to a replacement symbol by the UGen.)

Each parameter entry takes up a single line and consists of three fields, separated by a comma: `:<parameter name>, <parameber label>, <parameter ID>`.

The parameter label is the unit of measurement (e.g. "dB", "ms", "%"); it can be an empty string!

The parameter ID is a hexidecimal number. For VST 2.x plugins it is the same as the parameter index, but for VST 3.x plugins it can be an arbitrary 32 bit integer.

**NOTE**: Program names can be empty strings; this means that empty lines after the `[programs]` section are significant and must not be ignored!

Each plugin can be referred to by one or more *keys*. The primary key always comes first in the list.

*EXAMPLE:*
```
[plugin]
id=6779416F
path=C:/Program Files/VSTPlugins/GVST/GChorus.dll
name=GChorus
vendor=GVST
category=Effect
version=1200
sdkversion=VST 2.4
inputs=2
outputs=2
flags=d
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
[plugins]\n
n=<number of plugins>
<plugin info #0>
<plugin info #1>
...
<plugin info #N-1>
```

Each `<plugin info>` entry has the same structure as in "Plugin info".


### Plugin cache file

Probing lots of (large) VST plugins can be a slow process. To speed up subsequent searches, the search results can be written to a cache file (see `/vst_search`), which is located in a hidden folder named *.VSTPlugin* in the user's home directory. The cache file itself is named *cache.ini* for 64-bit servers and *cache32.ini* for 32-bit servers.

The cache file structure is very similar to that in "Search results". The only difference is that it also contains a black-list (marked by `[ignore]`).

```
[plugins]\n
n=<number of plugins>
<plugin info #0>
<plugin info #1>
...
<plugin info #N-1>
[ignore]
n=<number of paths>
<path #0>\n
<path #1>\n
...
<path #N-1>\n
```

Plugins are black-listed if the probe process failed (see `/vst_probe`). If you want to replace a "bad" plugin with a "good" one, you have to first remove the cache file (see `/vst_clear`).
