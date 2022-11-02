#include "PluginServer.h"

#include "ShmInterface.h"
#include "PluginDictionary.h"
#include "Log.h"
#include "FileUtils.h"
#include "MiscUtils.h"

#include <cassert>
#include <cstring>
#include <sstream>

#if DEBUG_SERVER_PROCESS
# define LOG_PROCESS(x) LOG_DEBUG(x)
#else
# define LOG_PROCESS(x)
#endif

namespace vst {

template<typename T>
void defer(const T& fn){
    // call on UI thread and catch exceptions
    bool success = false;
    Error err;
    bool ok = UIThread::callSync([&](){
        try {
            fn();
            success = true;
        } catch (const Error& e){
            err = e;
        } catch (const std::exception& e) {
            err = Error(e.what());
        }
    });
    if (ok){
        if (!success){
            throw err;
        }
        return;
    } else {
        LOG_ERROR("UIThread::callSync() failed");
    }
}

/*///////////////// PluginHandle ///////////////*/

PluginHandle::PluginHandle(PluginServer& server, IPlugin::ptr plugin,
                           uint32_t id, ShmChannel& channel)
    : server_(&server), plugin_(std::move(plugin)), id_(id)
{
    LOG_DEBUG("PluginHandle::PluginHandle (" << id_ << ")");
    // cache param state and send to client
    channel.clear(); // !

    auto nparams = plugin_->info().numParameters();
    if (nparams > 0){
        paramState_ = std::make_unique<float[]>(nparams);
    }
    for (int i = 0; i < nparams; ++i){
        auto value = plugin_->getParameter(i);
        paramState_[i] = value;
        sendParam(channel, i, value, false);
    }
}

void PluginHandle::init() {
    // set listener
    // NOTE: can't call this in the constructor!
    LOG_DEBUG("PluginHandle (" << id_ << "): set listener");
    plugin_->setListener(shared_from_this());
}

PluginHandle::~PluginHandle() {
    LOG_DEBUG("PluginHandle::~PluginHandle (" << id_ << ")");
}

void PluginHandle::handleRequest(const ShmCommand &cmd,
                                 ShmChannel &channel)
{
    // LOG_DEBUG("PluginHandle (" << id_ << "): got request " << cmd.type);
    switch (cmd.type){
    case Command::Process:
        // LOG_DEBUG("PluginHandle (" << id_ << "): process");
        process(cmd, channel);
        break;
    case Command::SetupProcessing:
    {
        LOG_DEBUG("PluginHandle (" << id_ << "): setupProcessing");
        maxBlockSize_ = cmd.setup.maxBlockSize;
        precision_ = static_cast<ProcessPrecision>(cmd.setup.precision);
        auto mode = static_cast<ProcessMode>(cmd.setup.mode);
        defer([&](){
            plugin_->setupProcessing(cmd.setup.sampleRate, maxBlockSize_,
                                     precision_, mode);
        });
        updateBuffer();
        break;
    }
    case Command::SetNumSpeakers:
    {
        LOG_DEBUG("PluginHandle (" << id_ << "): setNumSpeakers");
        int numInputs = cmd.speakers.numInputs;
        int numOutputs = cmd.speakers.numOutputs;
        auto speakers = cmd.speakers.speakers;
        auto input = (int *)alloca(sizeof(int) * numInputs);
        std::copy(speakers, speakers + numInputs, input);
        auto output = (int *)alloca(sizeof(int) * numOutputs);
        std::copy(speakers + numInputs,
                  speakers + numInputs + numOutputs, output);

        defer([&](){
            plugin_->setNumSpeakers(input, numInputs, output, numOutputs);
        });

        // create input busses
        inputs_ = numInputs > 0 ? std::make_unique<Bus[]>(numInputs) : nullptr;
        numInputs_ = numInputs;
        for (int i = 0; i < numInputs; ++i){
            inputs_[i] = Bus(input[i]);
        }
        // create output busses
        outputs_ = numOutputs > 0 ? std::make_unique<Bus[]>(numOutputs) : nullptr;
        numOutputs_ = numOutputs;
        for (int i = 0; i < numOutputs; ++i){
            outputs_[i] = Bus(output[i]);
        }

        updateBuffer();

        // send actual speaker arrangement
        channel.clear(); // !

        int size = sizeof(uint32_t) * (numInputs_ + numOutputs_);
        auto totalSize = CommandSize(ShmCommand, speakers, size);
        auto reply = (ShmCommand *)alloca(totalSize);
        reply->type = Command::SpeakerArrangement;
        reply->speakers.numInputs = numInputs;
        reply->speakers.numOutputs = numOutputs;
        // copy input and output arrangements
        for (int i = 0; i < numInputs; ++i){
            reply->speakers.speakers[i] = input[i];
        }
        for (int i = 0; i < numOutputs; ++i){
            reply->speakers.speakers[i + numInputs] = output[i];
        }

        addReply(channel, reply, totalSize);

        break;
    }
    case Command::Suspend:
        LOG_DEBUG("PluginHandle (" << id_ << "): suspend");
        defer([&](){
            plugin_->suspend();
        });
        break;
    case Command::Resume:
        LOG_DEBUG("PluginHandle (" << id_ << "): resume");
        defer([&](){
            plugin_->resume();
        });
        break;
    case Command::ReadProgramFile:
        LOG_DEBUG("PluginHandle (" << id_ << "): ReadProgramFile");
        defer([&](){
            plugin_->readProgramFile(cmd.buffer.data);
        });
        channel.clear(); // !
        sendParameterUpdate(channel);
        sendProgramUpdate(channel, false);
        break;
    case Command::ReadBankFile:
        LOG_DEBUG("PluginHandle (" << id_ << "): ReadBankFile");
        defer([&](){
            plugin_->readBankFile(cmd.buffer.data);
        });
        channel.clear(); // !
        sendParameterUpdate(channel);
        sendProgramUpdate(channel, true);
        break;
    case Command::ReadProgramData:
    {
        LOG_DEBUG("PluginHandle (" << id_ << "): ReadProgramData");
        auto realSize = cmd.i;
        // data is in a seperate message!
        const void *data;
        size_t size;
        if (channel.getMessage(data, size)){
            assert(size >= realSize); // 'size' can be larger because of padding!
        } else {
            throw Error(Error::PluginError,
                        "PluginClient::ReadProgramData: missing data message");
        }

        defer([&](){
            plugin_->readProgramData((const char *)data, realSize);
        });

        channel.clear(); // !

        sendParameterUpdate(channel);
        sendProgramUpdate(channel, false);
        break;
    }
    case Command::ReadBankData:
    {
        LOG_DEBUG("PluginHandle (" << id_ << "): ReadBankData");
        auto realSize = cmd.i;
        // data is in a seperate message!
        const void *data;
        size_t size;
        if (channel.getMessage(data, size)){
            assert(size >= realSize); // 'size' can be larger because of padding!
        } else {
            throw Error(Error::PluginError,
                        "PluginClient::ReadBankData: missing data message");
        }

        defer([&](){
            plugin_->readBankData((const char *)data, realSize);
        });

        channel.clear(); // !

        sendParameterUpdate(channel);
        sendProgramUpdate(channel, true);
        break;
    }
    case Command::WriteProgramFile:
        LOG_DEBUG("PluginHandle (" << id_ << "): WriteProgramFile");
        defer([&](){
            plugin_->writeProgramFile(cmd.buffer.data);
        });
        break;
    case Command::WriteBankFile:
        LOG_DEBUG("PluginHandle (" << id_ << "): WriteBankFile");
        defer([&](){
            plugin_->writeBankFile(cmd.buffer.data);
        });
        break;
    case Command::WriteProgramData:
    case Command::WriteBankData:
    {
        LOG_DEBUG("PluginHandle (" << id_ << "): WriteProgramData/WriteBankData");
        // get data
        std::string buffer;
        defer([&](){
            if (cmd.type == Command::WriteBankData){
                plugin_->writeBankData(buffer);
            } else {
                plugin_->writeProgramData(buffer);
            }
        });
        // send data
        channel.clear(); // !

        auto totalSize = sizeof(ShmCommand) + buffer.size();
        if (totalSize > channel.capacity()) {
            // plugin data too large for NRT channel, try to transmit via tmp file
            LOG_DEBUG("PluginHandle (" << id_ << "): send plugin data via tmp file (size: "
                      << buffer.size() << ", capacity: " << channel.capacity() << ")");
            std::stringstream ss;
            ss << getTmpDirectory() << "/vst_" << (void *)this;
            std::string path = ss.str();
            // NOTE: the file must be deleted by the client!
            File file(path, File::WRITE);
            if (!file){
                throw Error(Error::SystemError,
                            "PluginHandle: couldn't create tmp file");
            }
            file.write(buffer.data(), buffer.size());
            if (!file){
                if (!removeFile(path)) {
                    LOG_ERROR("PluginHandle: can't remove tmp file");
                }
                throw Error(Error::SystemError,
                            "PluginHandle: couldn't write plugin data to tmp file");
            }

            auto pathLength = path.size() + 1;
            auto replySize = CommandSize(ShmCommand, buffer, pathLength);
            auto reply = (ShmCommand *)alloca(replySize);
            new (reply) ShmCommand(Command::PluginDataFile, id_);
            memcpy(reply->buffer.data, path.data(), pathLength);
            reply->buffer.size = pathLength;

            addReply(channel, reply, replySize);
        } else {
            ShmCommand reply(Command::PluginData, id_);
            reply.i = buffer.size(); // save actual size!
            addReply(channel, &reply, sizeof(reply));

            // send actual data as a seperate message to avoid needless copy.
            if (!addReply(channel, buffer.data(), buffer.size())){
                throw Error("plugin data too large!"); // shouldn't happen
            }
        }

        break;
    }
    default:
        LOG_ERROR("PluginHandle (" << id_ << "): unknown NRT request " << cmd.type);
        break;
    }
}

void PluginHandle::handleUICommand(const ShmUICommand &cmd){
    auto window = plugin_->getWindow();
    if (window){
        switch (cmd.type){
        case Command::WindowOpen:
            LOG_DEBUG("WindowOpen");
            window->open();
            break;
        case Command::WindowClose:
            LOG_DEBUG("WindowClose");
            window->close();
            break;
        case Command::WindowSetPos:
            LOG_DEBUG("WindowSetPos");
            window->setPos(cmd.windowPos.x, cmd.windowPos.y);
            break;
        case Command::WindowSetSize:
            LOG_DEBUG("WindowSetSize");
            window->setSize(cmd.windowSize.width, cmd.windowSize.height);
            break;
        default:
            LOG_ERROR("PluginHandle (" << id_ << "): unknown UI command " << cmd.type);
            break;
        }
    } else {
        LOG_ERROR("PluginHandle (" << id_ << "): can't handle UI command without window!");
    }
}

void PluginHandle::parameterAutomated(int index, float value) {
    if (UIThread::isCurrentThread()){
        LOG_DEBUG("UI thread: ParameterAutomated");
        ShmUICommand cmd(Command::ParamAutomated, id_);
        cmd.paramAutomated.index = index;
        cmd.paramAutomated.value = value;

        // UI queue is bounded! For now, just sleep until
        // the other side has drained the queue...
        // Well behaved plugins shouldn't really overflow the queue anyway.
        int counter = 0;
        while (!server_->postUIThread(cmd)){
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            counter++;
            if (counter > 1000) {
                LOG_WARNING("PluginHandle (" << id_ << "): postUIThread() blocked for over 1 second");
                break;
            }
        }
        paramAutomated_.emplace(index, value);
    } else {
        LOG_DEBUG("RT thread: ParameterAutomated");
        Command cmd(Command::ParamAutomated);
        cmd.paramAutomated.index = index;
        cmd.paramAutomated.value = value;

        events_.push_back(cmd);
    }
}

void PluginHandle::latencyChanged(int nsamples) {
    if (UIThread::isCurrentThread()){
        LOG_DEBUG("UI thread: LatencyChanged");
        ShmUICommand cmd(Command::LatencyChanged, id_);
        cmd.latency = nsamples;

        // UI queue is bounded!
        if (!server_->postUIThread(cmd)) {
            LOG_WARNING("PluginHandle (" << id_ << "): couldn't post latency change!");
        }
    } else {
        Command cmd(Command::LatencyChanged);
        cmd.i = nsamples;

        events_.push_back(cmd);
    }
}

void PluginHandle::updateDisplay() {
    // don't send yet! we first need to update the parameter
    // cache and send the new values to the client.
    updateDisplay_.store(true);
}

void PluginHandle::midiEvent(const MidiEvent& event) {
    if (UIThread::isCurrentThread()){
        // ignore for now
    } else {
        Command cmd(Command::MidiReceived);
        cmd.midi = event;

        events_.push_back(cmd);
    }
}

void PluginHandle::sysexEvent(const SysexEvent& event) {
    if (UIThread::isCurrentThread()){
        // ignore for now
    } else {
        // deep copy!
        auto data = new char[event.size];
        memcpy(data, event.data, event.size);

        Command cmd(Command::SysexReceived);
        cmd.sysex.data = data;
        cmd.sysex.size = event.size;
        cmd.sysex.delta = event.delta;

        events_.push_back(cmd);
    }
}

void PluginHandle::updateBuffer(){
    int total = 0;
    for (int i = 0; i < numInputs_; ++i){
        total += inputs_[i].numChannels;
    }
    for (int i = 0; i < numOutputs_; ++i){
        total += outputs_[i].numChannels;
    }
    const int incr = maxBlockSize_ *
        (precision_ == ProcessPrecision::Double ? sizeof(double) : sizeof(float));
    buffer_.clear(); // force zero initialization
    buffer_.resize(total * incr);
    // set buffer vectors
    auto buf = buffer_.data();
    auto setBuffers = [](auto& busses, int count, auto& bufptr, int incr){
        for (int i = 0; i < count; ++i){
            auto& bus = busses[i];
            for (int j = 0; j < bus.numChannels; ++j){
                bus.channelData32[j] = (float *)bufptr; // float* and double* have the same size
                bufptr += incr;
            }
        }
    };
    setBuffers(inputs_, numInputs_, buf, incr);
    setBuffers(outputs_, numOutputs_, buf, incr);

    assert((buf - buffer_.data()) == buffer_.size());
}

void PluginHandle::process(const ShmCommand &cmd, ShmChannel &channel){
    // how to handle channel numbers vs speaker numbers?
    if (precision_ == ProcessPrecision::Double){
        doProcess<double>(cmd, channel);
    } else {
        doProcess<float>(cmd, channel);
    }
}

template<typename T>
void PluginHandle::doProcess(const ShmCommand& cmd, ShmChannel& channel){
    LOG_PROCESS("PluginHandle (" << id_ << "): start processing");

    assert(cmd.process.numInputs == numInputs_);
    assert(cmd.process.numOutputs == numOutputs_);

    ProcessData data;
    data.numSamples = cmd.process.numSamples;
    data.precision = static_cast<ProcessPrecision>(cmd.process.precision);
    data.mode = static_cast<ProcessMode>(cmd.process.mode);
    data.numInputs = numInputs_;
    data.inputs = inputs_.get();
    data.numOutputs = numOutputs_;
    data.outputs = outputs_.get();

    // read audio input data
    for (int i = 0; i < data.numInputs; ++i){
        auto& bus = data.inputs[i];
        LOG_PROCESS("PluginHandle (" << id_ << "): read input bus " << i << " with "
                     << bus.numChannels << " channels");
        // read channels
        for (int j = 0; j < bus.numChannels; ++j){
            auto chn = (T *)bus.channelData32[j];
            const void* msg;
            size_t size;
            if (channel.getMessage(msg, size)){
                // size can be larger because of message
                // alignment - don't use in std::copy!
                assert(size >= data.numSamples * sizeof(T));
                auto buf = (const float *)msg;
                std::copy(buf, buf + data.numSamples, chn);
            } else {
                std::fill(chn, chn + data.numSamples, 0);
                LOG_ERROR("PluginClient: missing channel " << j
                          << " for audio input bus " << i);
            }
        }
    }

    // read and dispatch commands
    LOG_PROCESS("PluginHandle (" << id_ << "): dispatch commands");
    dispatchCommands(channel);

    // process audio
    LOG_PROCESS("PluginHandle (" << id_ << "): process");
    plugin_->process(data);

    // send audio output data
    channel.clear(); // !

    // send output busses
    for (int i = 0; i < data.numOutputs; ++i){
        auto& bus = data.outputs[i];
        LOG_PROCESS("PluginHandle (" << id_ << "): write output bus " << i << " with "
                     << bus.numChannels << " channels");
        // write all channels sequentially to avoid additional copying.
        for (int j = 0; j < bus.numChannels; ++j){
            channel.addMessage(bus.channelData32[j], sizeof(T) * data.numSamples);
        }
    }

    // send events
    LOG_PROCESS("PluginHandle (" << id_ << "): send events");
    sendEvents(channel);

    // handle possible display update
    if (updateDisplay_.exchange(false)){
        sendParameterUpdate(channel);

        ShmCommand cmd(Command::UpdateDisplay);
        addReply(channel, &cmd, sizeof(ShmCommand));
    }

    LOG_PROCESS("PluginHandle (" << id_ << "): finished processing");
}

void PluginHandle::dispatchCommands(ShmChannel& channel){
    const void *data;
    size_t size;
    while (channel.getMessage(data, size)){
        auto cmd = (const ShmCommand *)data;
        switch(cmd->type){
        case Command::SetParamValue:
            plugin_->setParameter(cmd->paramValue.index, cmd->paramValue.value,
                                  cmd->paramValue.offset);
            {
                // parameter update event
                Command event(Command::ParameterUpdate);
                event.paramAutomated.index = cmd->paramValue.index;
                event.paramAutomated.value = cmd->paramValue.value;
                events_.push_back(event);
            }
            break;
        case Command::SetParamString:
            if (plugin_->setParameter(cmd->paramString.index, cmd->paramString.display,
                                  cmd->paramString.offset))
            {
                // parameter update event
                Command event(Command::ParameterUpdate);
                int index = cmd->paramValue.index;
                event.paramAutomated.index = index;
                event.paramAutomated.value = plugin_->getParameter(index);
                events_.push_back(event);
            }
            break;
        case Command::SetProgramName:
            plugin_->setProgramName(cmd->s);
            break;
        case Command::SetBypass:
            plugin_->setBypass(static_cast<Bypass>(cmd->i));
            break;
        case Command::SetTempo:
            plugin_->setTempoBPM(cmd->d);
            break;
        case Command::SetTimeSignature:
            plugin_->setTimeSignature(cmd->timeSig.num, cmd->timeSig.denom);
            break;
        case Command::SetTransportPlaying:
            plugin_->setTransportPlaying(cmd->i);
            break;
        case Command::SetTransportRecording:
            plugin_->setTransportRecording(cmd->i);
            break;
        case Command::SetTransportAutomationWriting:
            plugin_->setTransportAutomationWriting(cmd->i);
            break;
        case Command::SetTransportAutomationReading:
            plugin_->setTransportAutomationReading(cmd->i);
            break;
        case Command::SetTransportCycleActive:
            plugin_->setTransportCycleActive(cmd->i);
            break;
        case Command::SetTransportCycleStart:
            plugin_->setTransportCycleStart(cmd->d);
            break;
        case Command::SetTransportCycleEnd:
            plugin_->setTransportCycleEnd(cmd->d);
            break;
        case Command::SetTransportPosition:
            plugin_->setTransportPosition(cmd->d);
            break;
        case Command::SendMidi:
            plugin_->sendMidiEvent(cmd->midi);
            break;
        case Command::SendSysex:
            {
                SysexEvent sysex;
                sysex.delta = cmd->sysex.delta;
                sysex.size = cmd->sysex.size;
                sysex.data =cmd->sysex.data;

                plugin_->sendSysexEvent(sysex);
            }
            break;
        case Command::SetProgram:
            plugin_->setProgram(cmd->i);
            {
                // program change event
                Command event(Command::SetProgram);
                event.i = cmd->i;
                events_.push_back(event);
            }
            break;
        default:
            LOG_ERROR("PluginHandle (" << id_ << "): unknown RT command " << cmd->type);
            break;
        }
    }
}

void PluginHandle::sendEvents(ShmChannel& channel){
    for (auto& event : events_){
        switch (event.type){
        case Command::ParamAutomated:
        case Command::ParameterUpdate:
        {
            auto index = event.paramAutomated.index;
            auto value = event.paramAutomated.value;
            paramState_[index] = value;
            sendParam(channel, index, value,
                      event.type == Command::ParamAutomated);
            break;
        }
        case Command::LatencyChanged:
            addReply(channel, &event, sizeof(ShmCommand));
            break;
        case Command::UpdateDisplay:
            addReply(channel, &event, sizeof(ShmCommand));
            break;
        case Command::MidiReceived:
            addReply(channel, &event, sizeof(ShmCommand));
            break;
        case Command::SysexReceived:
        {
            auto size = CommandSize(ShmCommand, sysex, event.sysex.size);
            auto reply = (ShmCommand *)alloca(size);
            reply->type = event.type;
            reply->sysex.delta = event.sysex.delta;
            reply->sysex.size = event.sysex.size;
            memcpy(reply->sysex.data, event.sysex.data, event.sysex.size);

            addReply(channel, reply, size);
            break;
        }
        case Command::SetProgram:
            sendParameterUpdate(channel);
            break;
        default:
            LOG_ERROR("bug PluginHandle::sendEvents");
            break;
        }
    }

    events_.clear(); // !

    // handle parameter automation from UI thread
    int count = 0;
    Param param;
    while (count < paramAutomationRateLimit &&
           paramAutomated_.pop(param)){
        // NOTE: this is only necessary to keep the parameter cache
        // in the client and server in sync with the plugin state.
        // The actual automation message is sent to the UI queue
        // in parameterAutomated()!
        //
        // Check if the value has changed to avoid redundant messages.
        // E.g. some bad plugins will send *hundreds* of paramter automation
        // notifications when the user loads a preset in the plugin UI.
        // (Good plugins send the UpdateDisplay instead.)
        if (paramState_[param.index] != param.value) {
            sendParam(channel, param.index, param.value, false);
            paramState_[param.index] = param.value;
        }

        count++;
    }
}

void PluginHandle::sendParameterUpdate(ShmChannel& channel){
    // compare new param state with cached one
    // and send all parameters that have changed
    auto numParams = plugin_->info().numParameters();
    for (int i = 0; i < numParams; ++i){
        auto value = plugin_->getParameter(i);
        if (value != paramState_[i]){
            sendParam(channel, i, value, false);
            paramState_[i] = value;
        }
    }
}

void PluginHandle::sendProgramUpdate(ShmChannel &channel, bool bank){
    auto sendProgramName = [&](int index, const std::string& name){
        auto size  = CommandSize(ShmCommand, programName, name.size() + 1);
        auto reply = (ShmCommand *)alloca(size);
        reply->type = Command::ProgramNameIndexed;
        reply->programName.index = index;
        memcpy(reply->programName.name, name.c_str(), name.size() + 1);

        addReply(channel, reply, size);
    };

    if (bank){
        // send program number
        ShmCommand reply(Command::ProgramNumber);
        reply.i = plugin_->getProgram();

        addReply(channel, &reply, sizeof(reply));

        // send all program names
        int numPrograms = plugin_->info().numPrograms();
        for (int i = 0; i < numPrograms; ++i){
            sendProgramName(i, plugin_->getProgramNameIndexed(i));
        }
    } else {
        // send current program name
        if (plugin_->info().numPrograms() > 0){
            sendProgramName(plugin_->getProgram(), plugin_->getProgramName());
        }
    }
}

void PluginHandle::sendParam(ShmChannel &channel, int index,
                             float value, bool automated)
{
    std::string display = plugin_->getParameterString(index);

    auto size  = CommandSize(ShmCommand, paramState, display.size() + 1);
    auto reply = (ShmCommand *)alloca(size);
    new (reply) ShmCommand(automated ? Command::ParamAutomated
                                     : Command::ParameterUpdate);
    reply->paramState.index = index;
    reply->paramState.value = value;
    memcpy(reply->paramState.display, display.c_str(), display.size() + 1);

    addReply(channel, reply, size);
}

bool PluginHandle::addReply(ShmChannel& channel, const void *cmd, size_t size){
    return channel.addMessage(cmd, size);
}

/*////////////////// PluginServer ////////////////*/

static PluginDictionary gPluginDict;

PluginServer::PluginServer(int pid, const std::string& shmPath)
{
    LOG_DEBUG("PluginServer: parent: " << pid << ", path: " << shmPath);

#if VST_HOST_SYSTEM == VST_WINDOWS
    parent_ = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!parent_){
        throw Error(Error::SystemError, "OpenProcess() failed: "
                    + errorMessage(GetLastError()));
    }
#else
    parent_ = pid;
#endif
    shm_ = std::make_unique<ShmInterface>();
    shm_->connect(shmPath);
    LOG_DEBUG("PluginServer: connected to shared memory interface");
    // check version (for now it must match exactly)
    int major, minor, patch;
    shm_->getVersion(major, minor, patch);
    if (major == VERSION_MAJOR && minor == VERSION_MINOR
          && patch == VERSION_PATCH) {
        LOG_DEBUG("version: " << major << "." << minor << "." << patch);
    } else {
       throw Error(Error::PluginError, "host app version mismatch");
    }
    // setup UI event loop
    LOG_DEBUG("PluginServer: setup event loop");
    UIThread::setup();
    // install UI poll function
    LOG_DEBUG("PluginServer: add UI poll function");
    pollFunction_ = UIThread::addPollFunction([](void *x){
        static_cast<PluginServer *>(x)->pollUIThread();
    }, this);

    // create threads for NRT + RT channels
    LOG_DEBUG("PluginServer: create threads");
    running_ = true;
    for (int i = Channel::NRT; i < shm_->numChannels(); ++i){
        auto thread = std::thread(&PluginServer::runThread,
                                  this, &shm_->getChannel(i));
        threads_.push_back(std::move(thread));
    }

    LOG_DEBUG("PluginServer: ready");
}

PluginServer::~PluginServer(){
    LOG_DEBUG("free PluginServer");

    UIThread::removePollFunction(pollFunction_);

    for (auto& thread : threads_){
        thread.join();
    }

    // properly destruct all remaining plugins
    // on the UI thread (in case the parent crashed)
    if (!plugins_.empty()){
        LOG_DEBUG("release remaining " << plugins_.size() << " plugins");
        defer([&](){
            plugins_.clear();
        });
        LOG_DEBUG("released plugins");
    }

#if VST_HOST_SYSTEM == VST_WINDOWS
    CloseHandle(parent_);
#endif
}

void PluginServer::run(){
    UIThread::run();
}

bool PluginServer::postUIThread(const ShmUICommand& cmd){
    // sizeof(cmd) is a bit lazy, but we don't care about size here
    auto& channel = shm_->getChannel(Channel::UISend);
    return channel.writeMessage(&cmd, sizeof(cmd));
}

void PluginServer::pollUIThread(){
    // poll UI events and dispatch to plugin
    auto& channel = shm_->getChannel(Channel::UIReceive);

    char buffer[64]; // larger than ShmCommand!
    size_t size = sizeof(buffer);
    // read all available events
    while (channel.readMessage(buffer, size)){
        auto cmd = (const ShmUICommand *)buffer;
        auto plugin = findPlugin(cmd->id);
        if (plugin){
            plugin->handleUICommand(*cmd);
        } else {
            // UI commands run asynchronously, so they can be "late"
            switch (cmd->type) {
            case Command::WindowOpen:
            case Command::WindowClose:
            case Command::WindowSetPos:
            case Command::WindowSetSize:
                break;
            default:
                LOG_ERROR("PluginServer::pollUIThread: couldn't find plugin "
                          << cmd->id << " for command " << cmd->type);
                break;
            }
        }
        size = sizeof(buffer); // reset size!
    }

    checkParentAlive();
}

void PluginServer::checkParentAlive(){
#if VST_HOST_SYSTEM == VST_WINDOWS
    bool alive = WaitForSingleObject(parent_, 0) == WAIT_TIMEOUT;
#else
    auto parent = getppid();
  #ifndef __WINE__
    bool alive = parent == parent_;
  #else
    // We can't do this on Wine, because we might have been
    // forked in a Wine launcher app.
    // At least we can check for 1 (= reparented to init).
    // NOTE that this is not 100% reliable, that's why we
    // don't use this method for the other hosts.
    bool alive = parent != 1;
  #endif
#endif
    if (!alive){
        LOG_WARNING("parent (" << parent_ << ") terminated!");
    #if VST_HOST_SYSTEM != VST_WINDOWS
        LOG_DEBUG("new parent ID: " << parent);
    #endif
        quit();
    }
}

void PluginServer::runThread(ShmChannel *channel){
    // raise thread priority for RT threads, but not for dedicated NRT thread!
    if (channel->name() != "nrt") {
        setThreadPriority(Priority::High);
    }

    // while running, wait for requests and dispatch to plugin
    // Quit command -> quit()
    while (running_.load(std::memory_order_relaxed)) {
        // LOG_DEBUG(channel->name() << ": wait");
        channel->wait();
        // LOG_DEBUG(channel->name() << ": wake up");

        channel->reset();

        const void *msg;
        size_t size;
        if (channel->getMessage(msg, size)){
            handleCommand(*channel, *reinterpret_cast<const ShmCommand *>(msg));
        } else if (running_) {
            LOG_ERROR("PluginServer: '" << channel->name()
                      << "': couldn't get message");
            // ?
            channel->postReply();
        }
    }
    LOG_DEBUG(channel->name() << ": quit");
}

void PluginServer::handleCommand(ShmChannel& channel,
                                 const ShmCommand &cmd){
    PluginHandle *plugin;

    try {
        switch (cmd.type){
        case Command::CreatePlugin:
            createPlugin(cmd.id, cmd.plugin.data,
                         cmd.plugin.size, channel);
            break;
        case Command::DestroyPlugin:
            destroyPlugin(cmd.id);
            break;
        case Command::Quit:
            quit();
            break;
        default:
            plugin = findPlugin(cmd.id);
            if (plugin){
                plugin->handleRequest(cmd, channel);
            } else {
                LOG_ERROR("PluginServer: couldn't find plugin " << cmd.id);
            }
            break;
        }
    } catch (const Error& e) {
        LOG_DEBUG("exception: " << e.what());
        channel.clear(); // !

        std::string msg = e.what();

        char buf[256]; // can't use alloca() in catch block
        auto size = CommandSize(ShmCommand, error, msg.size() + 1);
        auto reply = new (buf) ShmCommand(Command::Error);
        reply->error.code = e.code();
        memcpy(reply->error.msg, msg.c_str(), msg.size() + 1);

        channel.addMessage(reply, size);
    }

    channel.postReply();
}

void PluginServer::createPlugin(uint32_t id, const char *data, size_t size,
                                ShmChannel& channel){
    LOG_DEBUG("PluginServer: create plugin " << id);

    PluginDesc::const_ptr info;
    if (size){
        // info is transmitted in place
        std::stringstream ss;
        ss << std::string(data, size);
        info = gPluginDict.readPlugin(ss);
    } else {
        // info is transmitted via a tmp file
        File file(data);
        if (!file.is_open()){
            throw Error(Error::PluginError, "couldn't read plugin info!");
        }
        info = gPluginDict.readPlugin(file);
    }

    LOG_DEBUG("PluginServer: did read plugin info");

    if (!info){
        // shouldn't happen...
        throw Error(Error::PluginError, "plugin info out of date!");
    }

    // create on UI thread!
    IPlugin::ptr plugin;
    defer([&](){
        // open with Mode::Native to avoid infinite recursion!
        plugin = info->create(true, false, RunMode::Native);
    });

    assert(plugin != nullptr);

    auto handle = std::make_shared<PluginHandle>(
                *this, std::move(plugin), id, channel);
    handle->init();

    WriteLock lock(pluginMutex_);
    plugins_.emplace(id, std::move(handle));
}

void PluginServer::destroyPlugin(uint32_t id){
    LOG_DEBUG("PluginServer: destroy plugin " << id);
    WriteLock lock(pluginMutex_);
    auto it = plugins_.find(id);
    if (it != plugins_.end()){
        auto plugin = it->second;
        plugins_.erase(it);
        lock.unlock();

        // release on the UI thread!
        // NOTE: we really have to release it in the function body,
        // so it's not enough to just use a move capture.
        defer([&plugin] () {
            plugin = nullptr;
        });
    } else {
        LOG_ERROR("PluginServer::destroyPlugin: chouldn't find plugin " << id);
    }
}

PluginHandle * PluginServer::findPlugin(uint32_t id){
    ReadLock lock(pluginMutex_);
    auto it = plugins_.find(id);
    if (it != plugins_.end()){
        return it->second.get();
    } else {
        return nullptr;
    }
}

void PluginServer::quit(){
    LOG_DEBUG("PluginServer: quit");

    running_.store(false);
    // wake up all threads
    for (int i = Channel::NRT; i < shm_->numChannels(); ++i){
        shm_->getChannel(i).post();
    }

    // quit event loop
    UIThread::quit();
}

} // vst
