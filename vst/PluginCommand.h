#pragma once

#include "Interface.h"

namespace vst {

// commands
struct Command {
    // type
    enum Type {
        // RT commands
        SetParamValue, // 0
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
        SetProgramName,
        // NRT commands
        CreatePlugin, // 17
        DestroyPlugin,
        Suspend,
        Resume,
        SetNumSpeakers,
        SetupProcessing,
        ReadProgramFile, // 23
        ReadProgramData,
        ReadBankFile,
        ReadBankData,
        WriteProgramFile,
        WriteProgramData,
        WriteBankFile,
        WriteBankData,
        // window
        WindowOpen, // 31
        WindowClose,
        WindowSetPos,
        WindowSetSize,
        // events/replies
        PluginData, // 35
        SpeakerArrangement,
        ProgramNumber,
        ProgramName,
        ProgramNameIndexed,
        ParameterUpdate, // 40
        ParamAutomated,
        LatencyChanged,
        UpdateDisplay,
        MidiReceived,
        SysexReceived,
        // for plugin bridge
        Error, // 46
        Process,
        Quit
    };
    Command(){}
    Command(Command::Type _type) : type(_type){}

    static const size_t headerSize = 8;

    // data
    // NOTE: the union needs to be 8 byte aligned
    // and we make the padding explicit.
    uint32_t type;
    uint32_t padding;

    union {
        // no data
        struct {} empty;
        // generic integer
        int32_t i;
        // generic float
        float f;
        // generic double
        double d;
        // string
        char *s;
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
    ShmCommand(Command::Type _type)
        : type(_type), id(0) {}
    ShmCommand(Command::Type _type, uint32_t _id)
        : type(_type), id(_id){}

    static const size_t headerSize = 8;

    // data
    // NOTE: the union needs to be 8 byte aligned, so we use
    // the additional space for the (optional) 'id' member.
    uint32_t type;
    uint32_t id;

    union {
        // no data
        struct {} empty;
        // generic integer
        int32_t i;
        // generic float
        float f;
        // generic double
        double d;
        // flat string
        char s[1];
        // generic buffer (e.g. preset data)
        struct {
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
            int32_t size;
            char data[1];
        } plugin;
        // process
        struct {
            uint16_t numSamples;
            uint8_t precision;
            uint8_t mode;
            uint16_t numInputs;
            uint16_t numOutputs;
        } process;
        // setup processing
        struct {
            float sampleRate;
            uint16_t maxBlockSize;
            uint8_t precision;
            uint8_t mode;
        } setup;
        // setup speakers
        struct {
            uint16_t numInputs;
            uint16_t numOutputs;
            uint32_t speakers[1];
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

// additional commands/replies (for IPC over shared memory)
// that are not covered by Command.
struct ShmUICommand {
    ShmUICommand(){}
    ShmUICommand(Command::Type _type, uint32_t _id)
        : type(_type), id(_id){}

    static const size_t headerSize = 8;

    uint32_t type = 0;
    uint32_t id = 0;

    union {
        // no data
        struct {} empty;
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
        // latency change
        int32_t latency;
    };
};

#define CommandSize(Class, field, extra) \
    (Class::headerSize + sizeof(Class::field) + extra)

} // vst
