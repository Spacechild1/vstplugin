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
        // NRT commands
        CreatePlugin,
        DestroyPlugin,
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
        // events/replies
        PluginData,
        ProgramNumber,
        ProgramName,
        ProgramNameIndexed,
        ParameterUpdate,
        ParamAutomated,
        LatencyChanged,
        MidiReceived,
        SysexReceived,
        // for plugin bridge
        Error,
        Process,
        Quit
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
        // param automated
        struct {
            int32_t index;
            float value;
        } paramAutomated;
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
        // midi
        MidiEvent midi;
        SysexEvent sysex;
    };
};

// additional commands/replies (for IPC over shared memory)
// that are not covered by Command.
struct ShmCommand {
    ShmCommand(){}
    ShmCommand(Command::Type _type) : type(_type){}

    static const size_t headerSize = 4;

    uint32_t type;

    union {
        // no data
        struct {} empty;
        // generic integer
        int32_t i;
        // generic float
        float f;
        // generic double
        double d;
        // plugin ID
        uint32_t id;
        // flat string
        char s[1];
        // generic buffer (e.g. preset data)
        struct {
            uint32_t id;
            int32_t size;
            char data[1];
        } buffer;
        // param value
        struct {
            int32_t offset;
            int32_t index;
            float value;
        } paramValue;
        // flat param string
        struct {
            int32_t offset;
            int32_t index;
            char display[1];
        } paramString;
        // flat param state
        struct {
            int32_t index;
            float value;
            char display[1];
        } paramState;
        // program name (indexed)
        struct {
            int32_t index;
            char name[1];
        } programName;
        // midi message
        MidiEvent midi;
        // flat sysex data
        struct {
            int32_t delta;
            int32_t size;
            char data[1];
        } sysex;
        // time signature
        struct {
            int32_t num;
            int32_t denom;
        } timeSig;
        // plugin
        struct {
            uint32_t id;
            int32_t size;
            char data[1];
        } plugin;
        // process
        struct {
            uint32_t id;
            uint8_t numInputs;
            uint8_t numOutputs;
            uint8_t numAuxInputs;
            uint8_t numAuxOutpus;
            uint32_t numSamples;
        } process;
        // setup processing
        struct {
            uint32_t id;
            double sampleRate;
            uint32_t maxBlockSize;
            uint32_t precision;
        } setup;
        // setup speakers
        struct {
            uint32_t id;
            uint16_t in;
            uint16_t out;
            uint16_t auxin;
            uint16_t auxout;
        } speakers;
        // error
        struct {
            int32_t code;
            char msg[1];
        } error;
    };

    void throwError() const {
        throw Error(static_cast<Error::ErrorCode>(error.code), error.msg);
    }
};

using ShmReply = ShmCommand;

// additional commands/replies (for IPC over shared memory)
// that are not covered by Command.
struct ShmUICommand {
    ShmUICommand(){}
    ShmUICommand(Command::Type _type, uint32_t _id)
        : type(_type), id(_id){}

    static const size_t headerSize = 8;

    uint32_t type;
    uint32_t id;

    union {
        // window position
        struct {
            int32_t x;
            int32_t y;
        } windowPos;
        // window size
        struct {
            int32_t width;
            int32_t height;
        } windowSize;
        // parameter automated
        struct {
            int32_t index;
            float value;
        } paramAutomated;
    };
};

#define CommandSize(Class, field, extra) \
    (Class::headerSize + sizeof(Class::field) + extra)

} // vst
