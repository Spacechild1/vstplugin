#pragma once

#include "Interface.h"

namespace vst {

struct PluginDesc final {
    static const uint32_t NoParamID = 0xffffffff;

    using ptr = std::shared_ptr<PluginDesc>;
    using const_ptr = std::shared_ptr<const PluginDesc>;

    PluginDesc(std::shared_ptr<const IFactory> f);
    ~PluginDesc();
    PluginDesc(const PluginDesc&) = delete;
    void operator =(const PluginDesc&) = delete;

    void setFactory(std::shared_ptr<const IFactory> factory);
    const std::string& path() const { return path_; }
    CpuArch arch() const;
    // create new instances
    // throws an Error exception on failure!
    IPlugin::ptr create(bool editor, bool threaded, RunMode mode = RunMode::Auto) const;
    // read/write plugin description
    void serialize(std::ostream& file) const;
    void deserialize(std::istream& file, int versionMajor = VERSION_MAJOR,
                     int versionMinor = VERSION_MINOR, int versionPatch = VERSION_PATCH);
#if USE_VST2
    void setUniqueID(int _id); // VST2
    int getUniqueID() const {
        return id_.id;
    }
#endif
#if USE_VST3
    void setUID(const char *uid); // VST3
    const char* getUID() const {
        return id_.uid;
    }
#endif
    struct SubPlugin {
        std::string name;
        int id;
    };
    using SubPluginList = std::vector<SubPlugin>;
    SubPluginList subPlugins;

    PluginType type() const { return type_; }
    // info data
    std::string uniqueID;
    std::string name;
    std::string vendor;
    std::string category;
    std::string version;
    std::string sdkVersion;

    struct Bus {
        enum Type {
            Main = 0,
            Aux
        };
        int numChannels = 0;
        Type type = Main;
        std::string label;
    };

    std::vector<Bus> inputs;
    int numInputs() const {
        return inputs.size();
    }

    std::vector<Bus> outputs;
    int numOutputs() const {
        return outputs.size();
    }

#if USE_VST3
    uint32_t programChange = NoParamID; // no param
    uint32_t bypass = NoParamID; // no param
#endif
    // parameters
    struct Param {
        std::string name;
        std::string label;
        uint32_t id = 0;
        bool automatable = true;
    };
    std::vector<Param> parameters;

    void addParameter(Param param);

    void addParamAlias(int index, const std::string& key){
        paramMap_[key] = index;
    }
    int findParam(const std::string& key) const;
    int numParameters() const {
        return parameters.size();
    }
#if USE_VST3
    // get VST3 parameter ID from index
    uint32_t getParamID(int index) const;
    // get index from VST3 parameter ID
    int getParamIndex(uint32_t _id) const;
#endif
    // presets
    void scanPresets();
    int numPresets() const { return presets.size(); }
    int findPreset(const std::string& name) const;
    Preset makePreset(const std::string& name, PresetType type = PresetType::User) const;
    int addPreset(Preset preset);
    bool removePreset(int index, bool del = true);
    bool renamePreset(int index, const std::string& newName);
    std::string getPresetFolder(PresetType type, bool create = false) const;
    PresetList presets;
    // default programs
    std::vector<std::string> programs;
    int numPrograms() const {
        return programs.size();
    }
    // flags
    enum Flags {
        HasEditor = 1 << 0,
        IsSynth = 1 << 1,
        SinglePrecision = 1 << 2,
        DoublePrecision = 1 << 3,
        MidiInput = 1 << 4,
        MidiOutput = 1 << 5,
        SysexInput = 1 << 6,
        SysexOutput = 1 << 7,
        Bridged = 1 << 8
    };
    bool editor() const {
        return flags & HasEditor;
    }
    bool synth() const {
        return flags & IsSynth;
    }
    bool singlePrecision() const {
        return flags & SinglePrecision;
    }
    bool doublePrecision() const {
        return flags & DoublePrecision;
    }
    bool hasPrecision(ProcessPrecision precision) const {
        if (precision == ProcessPrecision::Double) {
            return doublePrecision();
        }
        else {
            return singlePrecision();
        }
    }
    bool midiInput() const {
        return flags & MidiInput;
    }
    bool midiOutput() const {
        return flags & MidiOutput;
    }
    bool sysexInput() const {
        return flags & SysexInput;
    }
    bool sysexOutput() const {
        return flags & SysexOutput;
    }
    bool bridged() const {
        return flags & Bridged;
    }
    uint32_t flags = 0;
#if WARN_VST3_PARAMETERS
    bool warnParameters = false;
#endif
 private:
    std::weak_ptr<const IFactory> factory_;
    std::string path_;
    // param name to param index
    std::unordered_map<std::string, int> paramMap_;
#if USE_VST3
    // param index to ID (VST3 only)
    std::unordered_map<int, uint32_t> indexToIdMap_;
    // param ID to index (VST3 only)
    std::unordered_map<uint32_t, int> idToIndexMap_;
#endif
    PluginType type_;
    union ID {
        char uid[16];
        int32_t id;
    };
    ID id_;
    // helper methods
    void sortPresets(bool userOnly = true);
    mutable bool didCreatePresetFolder = false;
};

} // vst
