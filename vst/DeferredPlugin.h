#pragma once

#include "Interface.h"

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
        auto buf = (char *)malloc(str.size() + 1);
        memcpy(buf, str.data(), str.size() + 1);

        Command command(Command::SetParamString);
        auto& param = command.paramString;
        param.index = index;
        param.string = buf;
        param.offset = sampleOffset;
        pushCommand(command);

        return true; // what shall we do?
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
        auto data = (char *)malloc(event.size);
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
        command.b = play;
        pushCommand(command);
    }

    void setTransportRecording(bool record) override {
        Command command(Command::SetTransportRecording);
        command.b = record;
        pushCommand(command);
    }

    void setTransportAutomationWriting(bool writing) override {
        Command command(Command::SetTransportAutomationWriting);
        command.b = writing;
        pushCommand(command);
    }

    void setTransportAutomationReading(bool reading) override {
        Command command(Command::SetTransportAutomationReading);
        command.b = reading;
        pushCommand(command);
    }

    void setTransportCycleActive(bool active) override {
        Command command(Command::SetTransportCycleActive);
        command.b = active;
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
    // commands
    struct Command {
        // type
        enum Type {
            SetParamValue,
            SetParamString,
            SetBypass,
            SetTempo,
            SetTimeSignature,
            SetTransportPlaying,
            SetTransportRecording,
            SetTransportAutomationWriting,
            SetTransportAutomationReading,
            SetTransportCycleActive,
            SetTransportCycleStart,
            SetTransportCycleEnd,
            SetTransportPosition,
            SendMidi,
            SendSysex,
            SetProgram
        } type;
        Command() = default;
        Command(Command::Type _type) : type(_type){}
        // data
        union {
            bool b;
            int i;
            float f;
            char *s;
            // param value
            struct {
                int index;
                float value;
                int offset;
            } paramValue;
            // param string
            struct {
                int index;
                int offset;
                char* string;
            } paramString;
            // time signature
            struct {
                int num;
                int denom;
            } timeSig;
            // bypass
            Bypass bypass;
            // midi
            MidiEvent midi;
            SysexEvent sysex;
        };
    };
    virtual void pushCommand(const Command& command) = 0;
};

} // vst
