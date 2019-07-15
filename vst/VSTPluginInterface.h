#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <functional>
#include <memory>

// for intptr_t
#ifdef _MSC_VER
#ifdef _WIN64
typedef __int64 intptr_t;
#else
typedef __int32 intptr_t;
#endif
typedef unsigned int uint32_t;
#else
#include <stdint.h>
#endif

namespace vst {

class IVSTPlugin;

class IVSTWindow {
 public:
    // call this once before you create any windows. not thread safe (yet)
    static void initialize();
    // make a new window
    static std::unique_ptr<IVSTWindow> create(IVSTPlugin &plugin);
    // poll the main loop (needed if the editor is in the main thread)
    static void poll();

    virtual ~IVSTWindow() {}

    virtual void* getHandle() = 0; // get system-specific handle to the window
	virtual void run() = 0; // run a message loop for this window
    virtual void quit() = 0; // post quit message

    virtual void setTitle(const std::string& title) = 0;
    virtual void setGeometry(int left, int top, int right, int bottom) = 0;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void minimize() = 0;
    virtual void restore() = 0; // un-minimize
    virtual void bringToTop() = 0;
    virtual void update() {}
};

struct VSTMidiEvent {
    VSTMidiEvent(char status = 0, char data1 = 0, char data2 = 0, int _delta = 0){
        data[0] = status; data[1] = data1; data[2] = data2; delta = _delta;
    }
    char data[3];
    int delta;
};

struct VSTSysexEvent {
    VSTSysexEvent(const char *_data, size_t _size, int _delta = 0)
        : data(_data, _size), delta(_delta){}
    template <typename T>
    VSTSysexEvent(T&& _data, int _delta = 0)
        : data(std::forward<T>(_data)), delta(_delta){}
    VSTSysexEvent() = default;
    std::string data;
    int delta;
};

class IVSTPluginListener {
 public:
    virtual ~IVSTPluginListener(){}
    virtual void parameterAutomated(int index, float value) = 0;
    virtual void midiEvent(const VSTMidiEvent& event) = 0;
    virtual void sysexEvent(const VSTSysexEvent& event) = 0;
};

enum class VSTProcessPrecision {
    Single,
    Double
};

class VSTPluginDesc;

class IVSTPlugin {
 public:
    virtual ~IVSTPlugin(){}

    virtual const VSTPluginDesc& info() const = 0;
    virtual std::string getPluginName() const = 0;
    virtual std::string getPluginVendor() const = 0;
    virtual std::string getPluginCategory() const = 0;
    virtual std::string getPluginVersion() const = 0;
    virtual std::string getSDKVersion() const = 0;
    virtual int getPluginUniqueID() const = 0;
    virtual int canDo(const char *what) const = 0;
    virtual intptr_t vendorSpecific(int index, intptr_t value, void *ptr, float opt) = 0;

    virtual void process(const float **inputs, float **outputs, int nsamples) = 0;
    virtual void processDouble(const double **inputs, double **outputs, int nsamples) = 0;
    virtual bool hasPrecision(VSTProcessPrecision precision) const = 0;
    virtual void setPrecision(VSTProcessPrecision precision) = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;
    virtual void setSampleRate(float sr) = 0;
    virtual void setBlockSize(int n) = 0;
    virtual int getNumInputs() const = 0;
    virtual int getNumOutputs() const = 0;
    virtual bool isSynth() const = 0;
    virtual bool hasTail() const = 0;
    virtual int getTailSize() const = 0;
    virtual bool hasBypass() const = 0;
    virtual void setBypass(bool bypass) = 0;
    virtual void setNumSpeakers(int in, int out) = 0;

    virtual void setListener(IVSTPluginListener *listener) = 0;

    virtual void setTempoBPM(double tempo) = 0;
    virtual void setTimeSignature(int numerator, int denominator) = 0;
    virtual void setTransportPlaying(bool play) = 0;
    virtual void setTransportRecording(bool record) = 0;
    virtual void setTransportAutomationWriting(bool writing) = 0;
    virtual void setTransportAutomationReading(bool reading) = 0;
    virtual void setTransportCycleActive(bool active) = 0;
    virtual void setTransportCycleStart(double beat) = 0;
    virtual void setTransportCycleEnd(double beat) = 0;
    virtual void setTransportPosition(double beat) = 0;
    virtual double getTransportPosition() const = 0;

    virtual int getNumMidiInputChannels() const = 0;
    virtual int getNumMidiOutputChannels() const = 0;
    virtual bool hasMidiInput() const = 0;
    virtual bool hasMidiOutput() const = 0;
    virtual void sendMidiEvent(const VSTMidiEvent& event) = 0;
    virtual void sendSysexEvent(const VSTSysexEvent& event) = 0;

    virtual void setParameter(int index, float value) = 0;
    virtual bool setParameter(int index, const std::string& str) = 0;
    virtual float getParameter(int index) const = 0;
    virtual std::string getParameterName(int index) const = 0;
    virtual std::string getParameterLabel(int index) const = 0;
    virtual std::string getParameterDisplay(int index) const = 0;
    virtual int getNumParameters() const = 0;

    virtual void setProgram(int index) = 0;
    virtual void setProgramName(const std::string& name) = 0;
    virtual int getProgram() const = 0;
    virtual std::string getProgramName() const = 0;
    virtual std::string getProgramNameIndexed(int index) const = 0;
    virtual int getNumPrograms() const = 0;

    virtual bool hasChunkData() const = 0;
    virtual void setProgramChunkData(const void *data, size_t size) = 0;
    virtual void getProgramChunkData(void **data, size_t *size) const = 0;
    virtual void setBankChunkData(const void *data, size_t size) = 0;
    virtual void getBankChunkData(void **data, size_t *size) const = 0;

    virtual bool readProgramFile(const std::string& path) = 0;
    virtual bool readProgramData(const char *data, size_t size) = 0;
    virtual bool readProgramData(const std::string& buffer) = 0;
    virtual void writeProgramFile(const std::string& path) = 0;
    virtual void writeProgramData(std::string& buffer) = 0;
    virtual bool readBankFile(const std::string& path) = 0;
    virtual bool readBankData(const char *data, size_t size) = 0;
    virtual bool readBankData(const std::string& buffer) = 0;
    virtual void writeBankFile(const std::string& path) = 0;
    virtual void writeBankData(std::string& buffer) = 0;

    virtual bool hasEditor() const = 0;
    virtual void openEditor(void *window) = 0;
    virtual void closeEditor() = 0;
    virtual void getEditorRect(int &left, int &top, int &right, int &bottom) const = 0;
};

class IVSTFactory;

enum VSTPluginFlags {
    HasEditor = 0,
    IsSynth,
    SinglePrecision,
    DoublePrecision,
    MidiInput,
    MidiOutput,
    SysexInput,
    SysexOutput
};

enum class ProbeResult {
    success,
    fail,
    crash,
    none
};

struct VSTPluginDesc {
	VSTPluginDesc() = default;
    VSTPluginDesc(const IVSTFactory& factory);
    VSTPluginDesc(const IVSTFactory& factory, const IVSTPlugin& plugin);
    void serialize(std::ostream& file) const;
    void deserialize(std::istream& file);
    bool valid() const {
        return probeResult == ProbeResult::success;
    }
    // data
    ProbeResult probeResult = ProbeResult::none;
    std::string path;
    std::string name;
    std::string vendor;
    std::string category;
    std::string version;
    int id = 0;
    int numInputs = 0;
    int numOutputs = 0;
    // parameters
    struct Param {
        std::string name;
        std::string label;
    };
    std::vector<Param> parameters;
    // param name to param index
    std::unordered_map<std::string, int> paramMap;
    // default programs
    std::vector<std::string> programs;
    // see VSTPluginFlags
    uint32_t flags = 0;
    // create new instances
    std::unique_ptr<IVSTPlugin> create() const;
 private:
    friend class VST2Plugin;
    friend class VST2Factory;
    friend class VST3Plugin;
    friend class VST3Factory;
    const IVSTFactory * factory_ = nullptr;
    struct ShellPlugin {
        std::string name;
        int id;
    };
    std::vector<ShellPlugin> shellPlugins_;
};

using VSTPluginDescPtr = std::shared_ptr<VSTPluginDesc>;

class IModule {
 public:
    static std::unique_ptr<IModule> load(const std::string& path);
    virtual ~IModule(){}
    virtual bool init() = 0; // VST3 only
    virtual bool exit() = 0; // VST3 only
    template<typename T>
    T getFnPtr(const char *name) const {
        return (T)doGetFnPtr(name);
    }
 protected:
    virtual void *doGetFnPtr(const char *name) const = 0;
};

class IVSTFactory {
 public:
    // expects an absolute path to the actual plugin file with or without extension
    static std::unique_ptr<IVSTFactory> load(const std::string& path);

    virtual ~IVSTFactory(){}
    // get a list of all available plugins (probed in a seperate process)
    virtual std::vector<VSTPluginDescPtr> plugins() const = 0;
    virtual int numPlugins() const = 0;
    virtual void probe() = 0;
    virtual bool isProbed() const = 0;
    virtual std::string path() const = 0;
    // create a new plugin instance
    virtual std::unique_ptr<IVSTPlugin> create(const std::string& name, bool probe = false) const = 0;
 protected:
    VSTPluginDescPtr probePlugin(const std::string& name, int shellPluginID = 0);
};

using IVSTFactoryPtr = std::unique_ptr<IVSTFactory>;

class VSTError : public std::exception {
 public:
    VSTError(const std::string& msg)
        : msg_(msg){}
    const char * what() const noexcept override {
        return msg_.c_str();
    }
 private:
    std::string msg_;
};

// recursively search 'dir' for VST plug-ins. for each plugin, the callback function is evaluated with the absolute path and basename.
void search(const std::string& dir, std::function<void(const std::string&, const std::string&)> fn);

// recursively search 'dir' for a VST plugin. returns empty string on failure
std::string find(const std::string& dir, const std::string& path);

const std::vector<std::string>& getDefaultSearchPaths();

const std::vector<const char *>& getPluginExtensions();

} // vst
