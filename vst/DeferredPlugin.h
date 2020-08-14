#pragma once

#include "Interface.h"
#include "PluginCommand.h"

#include <cstring>

namespace vst {

class DeferredPlugin : public IPlugin {
 public:
    void setParameter(int index, float value, int sampleOffset) override {
        Command command(Command::SetParamValue);
        auto& param = command.paramValue;
        param.index = index;
        param.value = value;
        param.offset = sampleOffset;
        pushCommand(command);
    }

    bool setParameter(int index, const std::string& str, int sampleOffset) override {
        // copy string (LATER improve)
        auto buf = new char[str.size() + 1];
        memcpy(buf, str.data(), str.size() + 1);

        Command command(Command::SetParamString);
        auto& param = command.paramString;
        param.index = index;
        param.display = buf;
        param.offset = sampleOffset;
        pushCommand(command);

        return true; // what shall we do?
    }

    void setBypass(Bypass state) {
        Command command(Command::SetBypass);
        command.bypass = state;
        pushCommand(command);
    }

    void setProgram(int program) override {
        Command command(Command::SetProgram);
        command.i = program;
        pushCommand(command);
    }

    void sendMidiEvent(const MidiEvent& event) override {
        Command command(Command::SendMidi);
        auto& midi = command.midi;
        memcpy(midi.data, event.data, sizeof(event.data));
        midi.delta = event.delta;
        midi.detune = event.detune;
        pushCommand(command);
    }

    void sendSysexEvent(const SysexEvent& event) override {
        // copy data (LATER improve)
        auto data = new char[event.size];
        memcpy(data, event.data, event.size);

        Command command(Command::SendSysex);
        auto& sysex = command.sysex;
        sysex.data = data;
        sysex.size = event.size;
        sysex.delta = event.delta;
        pushCommand(command);
    }

    void setTempoBPM(double tempo) override {
        Command command(Command::SetTempo);
        command.f = tempo;
        pushCommand(command);
    }

    void setTimeSignature(int numerator, int denominator) override {
        Command command(Command::SetTransportPosition);
        command.timeSig.num = numerator;
        command.timeSig.denom = denominator;
        pushCommand(command);
    }

    void setTransportPlaying(bool play) override {
        Command command(Command::SetTransportPosition);
        command.i = play;
        pushCommand(command);
    }

    void setTransportRecording(bool record) override {
        Command command(Command::SetTransportRecording);
        command.i = record;
        pushCommand(command);
    }

    void setTransportAutomationWriting(bool writing) override {
        Command command(Command::SetTransportAutomationWriting);
        command.i = writing;
        pushCommand(command);
    }

    void setTransportAutomationReading(bool reading) override {
        Command command(Command::SetTransportAutomationReading);
        command.i = reading;
        pushCommand(command);
    }

    void setTransportCycleActive(bool active) override {
        Command command(Command::SetTransportCycleActive);
        command.i = active;
        pushCommand(command);
    }

    void setTransportCycleStart(double beat) override {
        Command command(Command::SetTransportCycleStart);
        command.f = beat;
        pushCommand(command);
    }

    void setTransportCycleEnd(double beat) override {
        Command command(Command::SetTransportCycleEnd);
        command.f = beat;
        pushCommand(command);
    }

    void setTransportPosition(double beat) override {
        Command command(Command::SetTransportPosition);
        command.f = beat;
        pushCommand(command);
    }
 protected:
    virtual void pushCommand(const Command& command) = 0;
};

} // vst
