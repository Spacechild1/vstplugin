#pragma once

#include "Interface.h"

namespace vst {

// commands
struct Command {
    // type
    enum Type {
        // RT commands
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
        SetProgram,
        // replies
        ProgramNumber,
        ProgramName,
        ParameterState,
        ReplyData,
        TransportPosition,
        // audio
        Process,
        // set plugin
        SetPlugin,
        // NRT commands
        Suspend,
        Resume,
        SetNumSpeakers,
        SetupProcessing,
        ReadProgramFile,
        ReadProgramData,
        ReadBankFile,
        ReadBankData,
        WriteProgramFile,
        WriteProgramData,
        WriteBankFile,
        WriteBankData,
        // window
        WindowOpen,
        WindowClose,
        WindowSetPos,
        WindowSetSize,
        // events
        ParamAutomated,
        LatencyChanged,
        MidiReceived,
        SysexReceived
    };
    Command() = default;
    Command(Command::Type _type) : type(_type){}

    static const size_t headerSize = 4;

    // data
    uint32_t type; // specify size!
    union {
        // no data
        struct {} empty;
        // generic integer
        int32_t i;
        // generic float
        float f;
        // generic double
        double d;
        // param value
        struct {
            int32_t offset;
            int32_t index;
            float value;
        } paramValue;
        // param string
        struct {
            int32_t offset;
            int32_t index;
            char* display;
        } paramString;
        // time signature
        struct {
            int32_t num;
            int32_t denom;
        } timeSig;
        // bypass
        Bypass bypass;
        // midi
        MidiEvent midi;
        SysexEvent sysex;
    };
};

// additional commands/replies (for IPC over shared memory)
// that are not covered by Command.
struct ShmCommand {
    ShmCommand() = default;
    ShmCommand(Command::Type _type) : type(_type){}

    static const size_t headerSize = 4;

    uint32_t type;

    union {
        // no data
        struct {} empty;
        // just a plugin ID
        uint32_t id;
        // flat param string
        struct {
            int32_t offset;
            int32_t index;
            char display[1];
        } paramString;
        // flat param state
        struct {
            int32_t offset;
            int32_t index;
            float value;
            char display[1];
        } paramState;
        // flat midi message
        struct {
            int32_t delta;
            float detune;
            char data[4];
        } midi;
        // flat sysex data
        struct {
            int32_t delta;
            int32_t size;
            char data[1];
        } sysex;
        // flat generic data (e.g. program data, file path)
        struct {
            int32_t size;
            char data[1];
        } buffer;
        // process
        struct {
            uint8_t numInputs;
            uint8_t numOutputs;
            uint8_t numAuxInputs;
            uint8_t numAuxOutpus;
            uint32_t numSamples;
        } process;
    };
};

// NRT commands that are not send during process
// and therefore need to carry the plugin ID
struct ShmNRTCommand {
    ShmNRTCommand() = default;
    ShmNRTCommand(Command::Type _type, uint32_t _id)
        : type(_type), id(_id){}

    static const size_t headerSize = 8;

    uint32_t type;
    uint32_t id;

    union {
        // no data
        struct {} empty;
        // setup processing
        struct {
            double sampleRate;
            uint32_t maxBlockSize;
            uint32_t precision;
        } setup;
        // setup speakers
        struct {
            uint16_t in;
            uint16_t out;
            uint16_t auxin;
            uint16_t auxout;
        } speakers;
        // generic buffer (e.g. preset data or file path)
        struct {
            int32_t size;
            char data[1];
        } buffer;
        // window
        struct {
            int32_t x;
            int32_t y;
        } windowPos;
        struct {
            int32_t width;
            int32_t height;
        } windowSize;
    };
};

} // vst
