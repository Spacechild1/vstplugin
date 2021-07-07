#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <functional>
#include <memory>

#include <stdint.h>

#define VST_WINDOWS 0
#define VST_MACOS 1
#define VST_LINUX 2

// overriden when building Wine!
#ifndef VST_HOST_SYSTEM
# if defined(_WIN32)
#  define VST_HOST_SYSTEM VST_WINDOWS
# elif defined(__APPLE__)
#  define VST_HOST_SYSTEM VST_MACOS
# elif defined(__linux__)
#  define VST_HOST_SYSTEM VST_LINUX
# else
#  error "unsupported host system"
# endif
#endif

#ifndef USE_VST2
#define USE_VST2 1
#endif

#ifndef USE_VST3
#define USE_VST3 1
#endif

#if defined(__WINE__)
# define USE_BRIDGE 1
#else
# ifndef USE_BRIDGE
#  define USE_BRIDGE 1
# endif
#endif

#if defined(__WINE__)
# undef USE_WINE
# define USE_WINE 0
#elif defined(_WIN32) && USE_WINE
# error "USE_WINE cannot be set on Windows!"
#else
# ifndef USE_WINE
#  define USE_WINE 0
# endif
#endif

// older clang versions don't ship std::filesystem
#ifndef USE_STDFS
# if defined(_WIN32)
#  define USE_STDFS 1
# else
#  define USE_STDFS 0
# endif
#endif

namespace vst {

const int VERSION_MAJOR = 0;
const int VERSION_MINOR = 5;
const int VERSION_PATCH = 0;
const int VERSION_PRERELEASE = 0;

const char * getVersionString();

using LogFunction = void (*)(int level, const char *);

void setLogFunction(LogFunction f);

void logMessage(int level, const char * msg);

void logMessage(int level, const std::string& msg);

struct MidiEvent {
    MidiEvent(char _status = 0, char _data1 = 0, char _data2 = 0,
              int _delta = 0, float _detune = 0){
        status = _status; data1 = _data1; data2 = _data2;
        delta = _delta; detune = _detune;
    }
    union {
        char data[4]; // explicit padding
        struct {
            char status;
            char data1;
            char data2;
        };
    };
    int32_t delta;
    float detune;
};

struct SysexEvent {
    SysexEvent(const char *_data = nullptr, size_t _size = 0, int _delta = 0)
        : data(_data), size(_size), delta(_delta){}
    const char *data;
    int32_t size;
    int32_t delta;
};

class IPluginListener {
 public:
    using ptr = std::shared_ptr<IPluginListener>;
    virtual ~IPluginListener(){}
    virtual void parameterAutomated(int index, float value) = 0;
    virtual void latencyChanged(int nsamples) = 0;
    virtual void midiEvent(const MidiEvent& event) = 0;
    virtual void sysexEvent(const SysexEvent& event) = 0;
    virtual void pluginCrashed() = 0;
};

enum class ProcessPrecision {
    Single,
    Double
};

enum class ProcessMode {
    Realtime,
    Offline
};

enum class Bypass {
    Off,
    Hard, // simply bypass (with cross-fade)
    Soft // let tails ring out
};

enum class PluginType {
    VST2,
    VST3
};

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool valid() const {
        return w > 0 && h > 0;
    }
};

struct PluginDesc;

class IWindow;

struct AudioBus {
    int numChannels;
    union {
        float **channelData32;
        double **channelData64;
    };
};

struct ProcessData {
    const AudioBus *inputs;
    AudioBus *outputs;
    int numInputs;
    int numOutputs;
    int numSamples;
    ProcessPrecision precision;
    ProcessMode mode;
};

void bypass(ProcessData& data);

class IPlugin {
 public:
    using ptr = std::unique_ptr<IPlugin>;
    using const_ptr = std::unique_ptr<const IPlugin>;

    virtual ~IPlugin(){}

    virtual const PluginDesc& info() const = 0;

    virtual void setupProcessing(double sampleRate, int maxBlockSize,
                                 ProcessPrecision precision, ProcessMode mode) = 0;
    virtual void process(ProcessData& data) = 0;
    virtual void suspend() = 0;
    virtual void resume() = 0;
    virtual void setBypass(Bypass state) = 0;
    virtual void setNumSpeakers(int *input, int numInputs, int *output, int numOutputs) = 0;
    virtual int getLatencySamples() = 0;

    virtual void setListener(IPluginListener::ptr listener) = 0;

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

    virtual void sendMidiEvent(const MidiEvent& event) = 0;
    virtual void sendSysexEvent(const SysexEvent& event) = 0;

    virtual void setParameter(int index, float value, int sampleOffset = 0) = 0;
    virtual bool setParameter(int index, const std::string& str, int sampleOffset = 0) = 0;
    virtual float getParameter(int index) const = 0;
    virtual std::string getParameterString(int index) const = 0;

    virtual void setProgram(int index) = 0;
    virtual void setProgramName(const std::string& name) = 0;
    virtual int getProgram() const = 0;
    virtual std::string getProgramName() const = 0;
    virtual std::string getProgramNameIndexed(int index) const = 0;

    // the following methods throw an Error exception on failure!
    virtual void readProgramFile(const std::string& path) = 0;
    virtual void readProgramData(const char *data, size_t size) = 0;
    void readProgramData(const std::string& buffer) {
        readProgramData(buffer.data(), buffer.size());
    }
    virtual void writeProgramFile(const std::string& path) = 0;
    virtual void writeProgramData(std::string& buffer) = 0;
    virtual void readBankFile(const std::string& path) = 0;
    virtual void readBankData(const char *data, size_t size) = 0;
    void readBankData(const std::string& buffer) {
        readBankData(buffer.data(), buffer.size());
    }
    virtual void writeBankFile(const std::string& path) = 0;
    virtual void writeBankData(std::string& buffer) = 0;

    virtual void openEditor(void *window) = 0;
    virtual void closeEditor() = 0;
    virtual bool getEditorRect(Rect& rect) const = 0;
    virtual void updateEditor() = 0;
    virtual void checkEditorSize(int& width, int& height) const = 0;
    virtual void resizeEditor(int width, int height) = 0;
    virtual bool canResize() const = 0;

    virtual void setWindow(std::unique_ptr<IWindow> window) = 0;
    virtual IWindow* getWindow() const = 0;

    // VST2 only
    virtual int canDo(const char *what) const { return 0; }
    virtual intptr_t vendorSpecific(int index, intptr_t value, void *p, float opt) { return 0; }
    // VST3 only
    virtual void beginMessage() {}
    virtual void addInt(const char* id, int64_t value) {}
    virtual void addFloat(const char* id, double value) {}
    virtual void addString(const char* id, const char *value) {}
    virtual void addString(const char* id, const std::string& value) {}
    virtual void addBinary(const char* id, const char *data, size_t size) {}
    virtual void endMessage() {}
};

class IFactory;

enum class PresetType {
    User,
    UserFactory,
    SharedFactory,
    Global
};

struct Preset {
    std::string name;
    std::string path;
    PresetType type;
};

using PresetList = std::vector<Preset>;

enum class RunMode {
    Auto,
    Sandbox,
    Native,
    Bridge
};

class IModule {
 public:
     // throws an Error exception on failure!
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

class Error : public std::exception {
 public:
    enum ErrorCode {
        NoError,
        Crash,
        SystemError,
        ModuleError,
        PluginError,
        UnknownError
    };

    Error(ErrorCode code = NoError)
        : code_(code) {}
    Error(const std::string& msg)
        : msg_(msg), code_(UnknownError) {}
    Error(ErrorCode code, const std::string& msg)
        : msg_(msg), code_(code) {}
    const char * what() const noexcept override {
        return msg_.c_str();
    }
    ErrorCode code() const noexcept {
        return code_;
    }
 private:
    std::string msg_;
    ErrorCode code_ = NoError;
};

struct ProbeResult {
    std::shared_ptr<PluginDesc> plugin;
    Error error;
    int index = 0;
    int total = 0;
    // methods
    bool valid() const { return error.code() == Error::NoError; }
};

enum class CpuArch;

class IFactory {
 public:
    using ptr = std::shared_ptr<IFactory>;
    using const_ptr = std::shared_ptr<const IFactory>;

    using ProbeCallback = std::function<void(const ProbeResult&)>;
    using ProbeFuture = std::function<bool(ProbeCallback)>;

    // expects an absolute path to the actual plugin file with or without extension
    // throws an Error exception on failure!
    static IFactory::ptr load(const std::string& path, bool probe = false);

    virtual ~IFactory() {}
    virtual void addPlugin(std::shared_ptr<PluginDesc> desc) = 0;
    virtual std::shared_ptr<const PluginDesc> getPlugin(int index) const = 0;
    virtual std::shared_ptr<const PluginDesc> findPlugin(const std::string& name) const = 0;
    virtual int numPlugins() const = 0;

    void probe(ProbeCallback callback, float timeout){
        probeAsync(timeout, false)(std::move(callback));
    }
    virtual ProbeFuture probeAsync(float timeout, bool nonblocking) = 0;
    virtual std::shared_ptr<const PluginDesc> probePlugin(int id) const = 0;

    bool valid() const { return numPlugins() > 0; }

    virtual const std::string& path() const = 0;
    virtual CpuArch arch() const = 0;
    // create a new plugin instance
    // throws an Error on failure!
    virtual IPlugin::ptr create(const std::string& name) const = 0;
};

class FactoryFuture {
public:
    FactoryFuture() = default;
    FactoryFuture(const std::string& path,
                  std::function<bool(IFactory::ptr&)>&& fn)
        : path_(path), fn_(std::move(fn)) {}
    const std::string& path() const { return path_; }
    bool operator()(IFactory::ptr& f){
        return fn_(f);
    }
private:
    std::string path_;
    std::function<bool(IFactory::ptr&)> fn_;
};

// recursively search 'dir' for VST plug-ins. for each plugin, the callback function is evaluated with the absolute path.
void search(const std::string& dir, std::function<void(const std::string&)> fn,
            bool filterByExtension = true, const std::vector<std::string>& excludePaths = {});

// recursively search 'dir' for a VST plugin. returns empty string on failure
std::string find(const std::string& dir, const std::string& path);

const std::vector<std::string>& getDefaultSearchPaths();

const std::vector<const char *>& getPluginExtensions();

bool hasPluginExtension(const std::string& path);

const char * getBundleBinaryPath();

class IWindow {
 public:
    using ptr = std::unique_ptr<IWindow>;
    using const_ptr = std::unique_ptr<const IWindow>;

    static IWindow::ptr create(IPlugin& plugin);

    virtual ~IWindow() {}

    // user methods
    virtual void open() = 0;
    virtual void close() = 0;
    virtual void setPos(int x, int y) = 0;
    virtual void setSize(int w, int h) = 0;

    // plugin methods
    virtual void update() {}
    virtual void resize(int w, int h) = 0;
};

namespace UIThread {
    void setup();

    // Run the event loop. This function must be called in the main thread.
    // It blocks until the event loop finishes.
    void run();
    // Ask the event loop to stop and terminate the program.
    // This function can be called from any thread.
    void quit();
    // Poll the event loop. This function must be called in the main thread.
    void poll();

    bool isCurrentThread();

    bool available();

    using Callback = void (*)(void *);

    bool sync();

    bool callSync(Callback cb, void *user);

    template<typename T>
    bool callSync(const T& fn){
        return callSync([](void *x){
            (*static_cast<const T *>(x))();
        }, (void *)&fn);
    }

    bool callAsync(Callback cb, void *user);

    using PollFunction = void (*)(void *);
    using Handle = int32_t;

    Handle addPollFunction(PollFunction fn, void *context);

    void removePollFunction(Handle handle);
}

} // vst
