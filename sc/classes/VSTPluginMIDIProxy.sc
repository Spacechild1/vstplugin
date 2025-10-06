// mimicks MIDIOut
VSTPluginMIDIProxy {
	var <>owner;
	// dummy variable for backwards compatibility with \midi Event type (use \vst_midi instead!)
	var <>uid;

	*new { arg theOwner;
		^super.new.owner_(theOwner);
	}

	// dummy setter/getter for compatibility with MIDIOut
	latency { ^0; }

	latency_ {
		"Calling 'latency' on VSTPluginMIDIProxy has no effect.\n"
		"If you want to schedule MIDI messages on the Server, use Server.bind or similar methods.".warn;
		^this;
	}

	write { arg len, hiStatus, loStatus, a=0, b=0, detune;
		owner.sendMidi(hiStatus bitOr: loStatus, a, b, detune);
	}

	writeMsg { arg len, hiStatus, loStatus, a=0, b=0, detune;
		^owner.sendMidiMsg(hiStatus bitOr: loStatus, a, b, detune);
	}

	noteOn { arg chan, note=60, veloc=64;
		this.write(3, 16r90, chan.asInteger, note.asInteger, veloc.asInteger, note.frac * 100);
	}

	noteOnMsg { arg chan, note=60, veloc=64;
		^this.writeMsg(3, 16r90, chan.asInteger, note.asInteger, veloc.asInteger, note.frac * 100);
	}

	noteOff { arg chan, note=60, veloc=64;
		this.write(3, 16r80, chan.asInteger, note.asInteger, veloc.asInteger, note.frac * 100);
	}

	noteOffMsg { arg chan, note=60, veloc=64;
		^this.writeMsg(3, 16r80, chan.asInteger, note.asInteger, veloc.asInteger, note.frac * 100);
	}

	polyTouch { arg chan, note=60, val=64;
		this.write(3, 16rA0, chan.asInteger, note.asInteger, val.asInteger);
	}

	polyTouchMsg { arg chan, note=60, val=64;
		^this.writeMsg(3, 16rA0, chan.asInteger, note.asInteger, val.asInteger);
	}

	control { arg chan, ctlNum=7, val=64;
		this.write(3, 16rB0, chan.asInteger, ctlNum.asInteger, val.asInteger);
	}

	controlMsg { arg chan, ctlNum=7, val=64;
		^this.writeMsg(3, 16rB0, chan.asInteger, ctlNum.asInteger, val.asInteger);
	}

	program { arg chan, num=1;
		this.write(2, 16rC0, chan.asInteger, num.asInteger);
	}

	programMsg { arg chan, num=1;
		^this.writeMsg(2, 16rC0, chan.asInteger, num.asInteger);
	}

	touch { arg chan, val=64;
		this.write(2, 16rD0, chan.asInteger, val.asInteger);
	}

	touchMsg { arg chan, val=64;
		^this.writeMsg(2, 16rD0, chan.asInteger, val.asInteger);
	}

	bend { arg chan, val=8192;
		val = val.asInteger;
		this.write(3, 16rE0, chan, val bitAnd: 127, val >> 7);
	}

	bendMsg { arg chan, val=8192;
		val = val.asInteger;
		^this.writeMsg(3, 16rE0, chan, val bitAnd: 127, val >> 7);
	}

	allNotesOff { arg chan;
		this.control(chan, 123, 0);
	}

	allNotesOffMsg { arg chan;
		^this.controlMsg(chan, 123, 0);
	}

	smpte	{ arg frames=0, seconds=0, minutes=0, hours=0, frameRate = 3;
		var packet;
		packet = [frames, seconds, minutes, hours]
			.asInteger
			.collect({ arg v, i; [(i * 2 << 4) | (v & 16rF), (i * 2 + 1 << 4) | (v >> 4) ] });
		packet = packet.flat;
		packet.put(7, packet.at(7) | ( frameRate << 1 ) );
		packet.do({ arg v; this.write(2, 16rF0, 16r01, v); });
	}

	// smpteMsg not practicable
	songPtr { arg songPtr;
		songPtr = songPtr.asInteger;
		this.write(4, 16rF0, 16r02, songPtr & 16r7f, songPtr >> 7 & 16r7f);
	}

	songPtrMsg { arg songPtr;
		songPtr = songPtr.asInteger;
		^this.writeMsg(4, 16rF0, 16r02, songPtr & 16r7f, songPtr >> 7 & 16r7f);
	}

	songSelect { arg song;
		this.write(3, 16rF0, 16r03, song.asInteger);
	}

	songSelectMsg { arg song;
		^this.writeMsg(3, 16rF0, 16r03, song.asInteger);
	}

	midiClock {
		this.write(1, 16rF0, 16r08);
	}

	midiClockMsg {
		^this.writeMsg(1, 16rF0, 16r08);
	}

	start {
		this.write(1, 16rF0, 16r0A);
	}

	startMsg {
		^this.writeMsg(1, 16rF0, 16r0A);
	}

	continue {
		this.write(1, 16rF0, 16r0B);
	}

	continueMsg {
		^this.writeMsg(1, 16rF0, 16r0B);
	}

	stop {
		this.write(1, 16rF0, 16r0C);
	}

	stopMsg {
		^this.writeMsg(1, 16rF0, 16r0C);
	}

	reset {
		this.write(1, 16rF0, 16r0F);
	}

	resetMsg {
		^this.writeMsg(1, 16rF0, 16r0F);
	}

	sysex { arg packet;
		owner.sendSysex(packet);
	}

	sysexMsg { arg packet;
		^owner.sendSysexMsg(packet);
	}
}
