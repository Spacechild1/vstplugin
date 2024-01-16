#pragma once

#include "Interface.h"

namespace vst {

// commands
struct Command {
    static constexpr size_t maxShortStringSize = 15;

    // type
    enum Type {
        // RT commands
        SetParamValue, // 0
        SetParamString,
        SetParamStringShort,
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
        CreatePlugin, // 18
        DestroyPlugin,
        Suspend,
        Resume,
        SetNumSpeakers,
        SetupProcessing,
        ReadProgramFile, // 24
        ReadProgramData,
        ReadBankFile,
        ReadBankData,
        WriteProgramFile,
        WriteProgramData,
        WriteBankFile,
        WriteBankData,
        // window
        WindowOpen, // 32
        WindowClose,
        WindowSetPos,
        WindowSetSize,
        // events/replies
        PluginData, // 36
        PluginDataFile,
        SpeakerArrangement,
        ProgramChange,
        ProgramNumber,
        ProgramName,
        ProgramNameIndexed,
        ParameterUpdate, // 43
        ParamAutomated,
        LatencyChanged,
        UpdateDisplay,
        MidiReceived,
        SysexReceived,
        // for plugin bridge
        Error, // 49
        Process,
        Quit
    };
    Command(){}
    Command(Command::Type _type) : type(_type){}

    static const size_t headerSize = 8;

    // NOTE: the union needs to be 8 byte aligned
    // and we make the padding explicit.
    uint32_t type;
    uint32_t padding;

    // NOTE: with a few exceptions, the union members are
    // layout compatible with the ones in ShmCommand.
    union {
        // no data
        struct {} empty;
        // generic integer
        int32_t i;
        // generic float
        float f;
        // generic double
        double d;
        // C string
        char *s;
        // param automated
        struct {
            int32_t index;
            float value;
        } paramAutomated;
        // param value
        struct {
            uint16_t offset;
            uint16_t index;
            float value;
        } paramValue;
        // param string
        struct {
            uint16_t offset;
            uint16_t index;
            uint32_t size;
            char* str;
        } paramString;
        // short param string
        // (avoid heap allocation, same size as paramString on 64-bit)
        struct {
            uint16_t offset;
            uint16_t index;
            uint8_t pstr[12]; // pascal string!
        } paramStringShort;
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
        // flat C string
        char s[1];
        // generic buffer (e.g. preset data)
        struct {
            int32_t size;
            char data[1];
        } buffer;
        // param value, for setParameter()
        struct {
            uint16_t offset;
            uint16_t index;
            float value;
        } paramValue;
        // flat param string, for setParameterString()
        struct {
            uint16_t offset;
            uint16_t index;
            uint8_t pstr[1]; // pascal string!
        } paramString;
        // flat param state, for parameter updates
        struct {
            float value;
            uint16_t index;
            uint8_t pstr[1]; // pascal string!
        } paramState;
        // program name (indexed)
        struct {
            uint16_t index;
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
