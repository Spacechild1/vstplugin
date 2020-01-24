#pragma once

#include "Interface.h"

#ifndef USE_PLATFORM_EVENT
#define USE_PLATFORM_EVENT 1
#endif

#if USE_PLATFORM_EVENT
#if defined(_WIN32)
  // Windows Event
  #include <windows.h>
#elif defined(__APPLE__)
  // macOS doesn't support unnamed pthread semaphores,
  // so we use Mach semaphores instead
  #include <dispatch/dispatch.h>
#else
  // unnamed pthread semaphore
  #include <semaphore.h>
#endif
#else // C++11 condition_variable
  #include <condition_variable>
#endif

#include <atomic>

namespace vst {

class Event {
 public:
    Event();
    ~Event();
    void signal();
    void wait();
 private:
#if USE_PLATFORM_EVENT
#if defined(_WIN32)
    HANDLE event_;
#elif defined(__APPLE__)
    dispatch_semaphore_t sem_;
#else // pthreads
    sem_t sem_;
#endif
#else // USE_PLATFORM_EVENT
    std::condition_variable condition_;
    std::mutex mutex_;
    bool state_ = false;
#endif
};

class ThreadedPlugin final : public IPlugin {
 public:
    ThreadedPlugin(IPlugin::ptr plugin);
    ~ThreadedPlugin();

    void lock();
    void unlock();

    PluginType getType() const override {
        return plugin_->getType();
    }

    const PluginInfo& info() const override {
        return plugin_->info();
    }

    void setupProcessing(double sampleRate, int maxBlockSize, ProcessPrecision precision) override;
    void process(ProcessData<float>& data) override;
    void process(ProcessData<double>& data) override;
    void suspend() override;
    void resume() override;
    void setBypass(Bypass state) override;
    void setNumSpeakers(int in, int out, int auxIn, int auxOut) override;

    void setListener(IPluginListener::ptr listener) override;

    void setTempoBPM(double tempo) override;
    void setTimeSignature(int numerator, int denominator) override;
    void setTransportPlaying(bool play) override;
    void setTransportRecording(bool record) override;
    void setTransportAutomationWriting(bool writing) override;
    void setTransportAutomationReading(bool reading) override;
    void setTransportCycleActive(bool active) override;
    void setTransportCycleStart(double beat) override;
    void setTransportCycleEnd(double beat) override;
    void setTransportPosition(double beat) override;
    double getTransportPosition() const override {
        return plugin_->getTransportPosition();
    }

    void sendMidiEvent(const MidiEvent& event) override;
    void sendSysexEvent(const SysexEvent& event) override;

    void setParameter(int index, float value, int sampleOffset = 0) override;
    bool setParameter(int index, const std::string& str, int sampleOffset = 0) override;
    float getParameter(int index) const override;
    std::string getParameterString(int index) const override;

    void setProgram(int program) override;
    void setProgramName(const std::string& name) override;
    int getProgram() const override;
    std::string getProgramName() const override;
    std::string getProgramNameIndexed(int index) const override;

    void readProgramFile(const std::string& path) override;
    void readProgramData(const char *data, size_t size) override;
    void writeProgramFile(const std::string& path) override;
    void writeProgramData(std::string& buffer) override;
    void readBankFile(const std::string& path) override;
    void readBankData(const char *data, size_t size) override;
    void writeBankFile(const std::string& path) override;
    void writeBankData(std::string& buffer) override;

    void openEditor(void *window) override {
        plugin_->openEditor(window);
    }
    void closeEditor() override {
        plugin_->closeEditor();
    }
    bool getEditorRect(int &left, int &top, int &right, int &bottom) const override {
        return plugin_->getEditorRect(left, top, right, bottom);
    }
    void updateEditor() override {
        plugin_->updateEditor();
    }
    void checkEditorSize(int &width, int &height) const override {
        plugin_->checkEditorSize(width, height);
    }
    void resizeEditor(int width, int height) override {
        plugin_->resizeEditor(width, height);
    }
    bool canResize() const override {
        return plugin_->canResize();
    }
    void setWindow(IWindow::ptr window) override {
        plugin_->setWindow(std::move(window));
    }
    IWindow *getWindow() const override {
        return plugin_->getWindow();
    }

    // VST2 only
    int canDo(const char *what) const override {
        return plugin_->canDo(what);
    }
    intptr_t vendorSpecific(int index, intptr_t value, void *p, float opt) override;

    // VST3 only
    void beginMessage() override {
        plugin_->beginMessage();
    }
    void addInt(const char* id, int64_t value) override {
        plugin_->addInt(id, value);
    }
    void addFloat(const char* id, double value) override {
        plugin_->addFloat(id, value);
    }
    void addString(const char* id, const char *value) override {
        plugin_->addString(id, value);
    }
    void addString(const char* id, const std::string& value) override {
        plugin_->addString(id, value);
    }
    void addBinary(const char* id, const char *data, size_t size) override {
        plugin_->addBinary(id, data, size);
    }
    void endMessage() override {
        plugin_->endMessage();
    }
 private:
    void updateBuffer();
    template<typename T>
    void doProcess(ProcessData<T>& data);
    void dispatchCommands();
    template<typename T>
    void threadFunction(int numSamples);
    // data
    IPlugin::ptr plugin_;
    mutable std::mutex mutex_;
    bool locked_ = false;
    Event event_;
    // commands
    struct Command {
        // type
        enum Type {
            SetParamValue,
            SetParamString,
            Suspend,
            Resume,
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
            ReadProgramData,
            ReadBankData
        } type;
        Command() = default;
        Command(Command::Type _type) : type(_type){}
        // data
        union {
            bool b;
            int i;
            float f;
            // param value
            struct {
                int index;
                float value;
                int offset;
            } paramValue;
            // param string
            struct {
                int index;
                char* string;
                int offset;
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
    void pushCommand(const Command& command){
        commands_[current_].push_back(command);
    }
    std::vector<Command> commands_[2];
    int current_ = 0;
    std::string data_[2];
    // buffer
    int blockSize_ = 0;
    ProcessPrecision precision_ = ProcessPrecision::Single;
    std::vector<void *> input_;
    std::vector<void *> auxInput_;
    std::vector<void *> output_;
    std::vector<void *> auxOutput_;
    std::vector<char> buffer_;
};

} // vst
