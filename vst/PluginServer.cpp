#include "PluginServer.h"
#include "ShmInterface.h"
#include "Utility.h"
#include "PluginManager.h"

#include <cassert>
#include <cstring>
#include <sstream>

namespace vst {

static void sleep(int s){
    std::this_thread::sleep_for(std::chrono::seconds(s));
}

/*///////////////// PluginHandle ///////////////*/

PluginHandle::PluginHandle(PluginServer& server, IPlugin::ptr plugin,
                           uint32_t id, ShmChannel& channel)
    : server_(&server), plugin_(std::move(plugin)), id_(id)
{
    // cache param state and send to client
    channel.clear(); // !

    auto nparams = plugin_->info().numParameters();

    paramState_.reset(new float[nparams]);

    for (int i = 0; i < nparams; ++i){
        auto value = plugin_->getParameter(i);
        paramState_[i] = value;
        sendParam(channel, i, value, false);
    }
}

void PluginHandle::handleRequest(const ShmCommand &cmd,
                                 ShmChannel &channel)
{
    LOG_DEBUG("PluginHandle: got request " << cmd.type);
    switch (cmd.type){
    case Command::Process:
        LOG_DEBUG("PluginHandle: process");
        process(cmd, channel);
        break;
    case Command::SetupProcessing:
        LOG_DEBUG("PluginHandle: setupProcessing");
        maxBlockSize_ = cmd.setup.maxBlockSize;
        precision_ = static_cast<ProcessPrecision>(cmd.setup.precision);
        plugin_->setupProcessing(cmd.setup.sampleRate,
                                 maxBlockSize_, precision_);
        updateBuffer();
        break;
    case Command::SetNumSpeakers:
        LOG_DEBUG("PluginHandle: setNumSpeakers");
        numInputs_ = cmd.speakers.in;
        numOutputs_ = cmd.speakers.out;
        numAuxInputs_ = cmd.speakers.auxin;
        numAuxOutputs_ = cmd.speakers.auxout;
        plugin_->setNumSpeakers(numInputs_, numOutputs_,
                                numAuxInputs_, numAuxOutputs_);
        updateBuffer();
        break;
    case Command::Suspend:
        LOG_DEBUG("PluginHandle: suspend");
        plugin_->suspend();
        break;
    case Command::Resume:
        LOG_DEBUG("PluginHandle: resume");
        plugin_->resume();
        break;
    case Command::ReadProgramFile:
        plugin_->readProgramFile(cmd.buffer.data);
        sendUpdate(channel, false);
        break;
    case Command::ReadBankFile:
        plugin_->readBankFile(cmd.buffer.data);
        sendUpdate(channel, false);
        break;
    case Command::ReadProgramData:
        plugin_->readProgramData(cmd.buffer.data, cmd.buffer.size);
        sendUpdate(channel, true);
        break;
    case Command::ReadBankData:
        plugin_->readBankData(cmd.buffer.data, cmd.buffer.size);
        sendUpdate(channel, true);
        break;
    case Command::WriteProgramFile:
        plugin_->writeProgramFile(cmd.buffer.data);
        break;
    case Command::WriteBankFile:
        plugin_->writeBankFile(cmd.buffer.data);
        break;
    case Command::WriteProgramData:
    case Command::WriteBankData:
    {
        // LATER improve
        std::string buffer;
        if (cmd.type == Command::WriteBankData){
            plugin_->writeBankData(buffer);
        } else {
            plugin_->writeProgramData(buffer);
        }

        auto size = CommandSize(ShmCommand, buffer, buffer.size());
        auto reply = (ShmCommand *)alloca(size);
        new (reply) ShmCommand(Command::PluginData, id_);
        reply->buffer.size = buffer.size();
        memcpy(reply->buffer.data, buffer.data(), buffer.size());

        channel.clear(); // !

        addReply(channel, reply, size);

        break;
    }
    default:
        LOG_ERROR("PluginHandle: unknown NRT request " << cmd.type);
        break;
    }
}

void PluginHandle::handleUICommand(const ShmUICommand &cmd){
    auto window = plugin_->getWindow();
    if (window){
        switch (cmd.type){
        case Command::WindowOpen:
            window->open();
            break;
        case Command::WindowClose:
            window->close();
            break;
        case Command::WindowSetPos:
            window->setPos(cmd.windowPos.x, cmd.windowPos.y);
            break;
        case Command::WindowSetSize:
            window->setSize(cmd.windowSize.width, cmd.windowSize.height);
            break;
        default:
            LOG_ERROR("PluginHandle: unknown UI command " << cmd.type);
            break;
        }
    } else {
        LOG_ERROR("PluginHandle: can't handle UI command without window!");
    }
}

void PluginHandle::parameterAutomated(int index, float value) {
    if (UIThread::isCurrentThread()){
        ShmUICommand cmd(Command::ParamAutomated, id_);
        cmd.paramAutomated.index = index;
        cmd.paramAutomated.value = value;

        server_->postUIThread(cmd);
    } else {
        Command cmd(Command::ParamAutomated);
        cmd.paramAutomated.index = index;
        cmd.paramAutomated.value = value;

        events_.push_back(cmd);
    }
}

void PluginHandle::latencyChanged(int nsamples) {
    if (UIThread::isCurrentThread()){
        // shouldn't happen
    } else {
        Command cmd(Command::LatencyChanged);
        cmd.i = nsamples;

        events_.push_back(cmd);
    }
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
    auto sampleSize = precision_ == ProcessPrecision::Double ?
                sizeof(double) : sizeof(float);
    auto totalSize = (numInputs_ + numOutputs_ + numAuxInputs_ + numAuxOutputs_)
            * maxBlockSize_ * sampleSize;
    buffer_.resize(totalSize);
}

void PluginHandle::process(const ShmCommand &cmd, ShmChannel &channel){
    if (precision_ == ProcessPrecision::Double){
        doProcess<double>(cmd.process.numSamples, channel);
    } else {
        doProcess<float>(cmd.process.numSamples, channel);
    }
}

template<typename T>
void PluginHandle::doProcess(int numSamples, ShmChannel& channel){
    IPlugin::ProcessData<T> data;
    data.numInputs = numInputs_;
    data.input = (const T**)alloca(sizeof(T*) * numInputs_);
    data.numOutputs = numOutputs_;
    data.output = (T**)alloca(sizeof(T*) * numOutputs_);
    data.numAuxInputs = numAuxInputs_;
    data.auxInput = (const T**)alloca(sizeof(T*) * numAuxInputs_);
    data.numAuxOutputs = numAuxOutputs_;
    data.auxOutput = (T**)alloca(sizeof(T*) * numAuxOutputs_);

    // read audio input data
    auto bufptr = buffer_.data();

    auto readAudio = [&](auto vec, auto numChannels){
        for (int i = 0; i < numChannels; ++i){
            const char *data;
            size_t size;
            if (channel.getMessage(data, size)){
                assert(size == numSamples * sizeof(T));
                memcpy(bufptr, data, size);
                vec[i] = (const T *)bufptr;
                bufptr += size;
            } else {
                LOG_ERROR("PluginHandle::doProcess");
            }
        }
    };

    readAudio(data.input, numInputs_);
    readAudio(data.auxInput, numAuxInputs_);

    // set output pointers
    for (int i = 0; i < numOutputs_; ++i){
        data.output[i] = (T *)bufptr;
        bufptr += sizeof(T) * numSamples;
    }
    for (int i = 0; i < numAuxOutputs_; ++i){
        data.auxOutput[i] = (T *)bufptr;
        bufptr += sizeof(T) * numSamples;
    }

    // read and dispatch commands
    dispatchCommands(channel);

    // process audio
    plugin_->process(data);

    // send audio output data
    channel.clear(); // !

    for (int i = 0; i < numOutputs_; ++i){
        channel.addMessage((const char *)data.output[i],
                           sizeof(T) * numSamples);
    }
    for (int i = 0; i < numAuxOutputs_; ++i){
        channel.addMessage((const char *)data.auxOutput[i],
                           sizeof(T) * numSamples);
    }

    // send replies
    sendEvents(channel);
}

void PluginHandle::dispatchCommands(ShmChannel& channel){
    const char *data;
    size_t size;
    while (channel.getMessage(data, size)){
        auto cmd = (const ShmCommand *)data;
        switch(cmd->type){
        case Command::SetParamValue:
            plugin_->setParameter(cmd->paramValue.index, cmd->paramValue.value,
                                  cmd->paramValue.offset);
            {
                Command event(Command::ParameterUpdate);
                event.paramAutomated.index = cmd->paramValue.index;
                event.paramAutomated.value = cmd->paramValue.value;
                events_.push_back(event);
            }
            break;
        case Command::SetParamString:
            plugin_->setParameter(cmd->paramString.index, cmd->paramString.display,
                                  cmd->paramString.offset);
            {
                Command event(Command::ParameterUpdate);
                int index = cmd->paramValue.index;
                event.paramAutomated.index = index;
                event.paramAutomated.value = plugin_->getParameter(index);
                events_.push_back(event);
            }
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
                // to send parameter updates
                Command event(Command::SetProgram);
                event.i = cmd->i;
                events_.push_back(event);
            }
            break;
        default:
            LOG_ERROR("PluginHandle: unknown RT command " << cmd->type);
            break;
        }
    }

    events_.clear(); // !
}

void PluginHandle::sendEvents(ShmChannel& channel){
    for (auto& event : events_){
        switch (event.type){
        case Command::ParamAutomated:
        case Command::ParameterUpdate:
            sendParam(channel, event.paramAutomated.index,
                      event.paramAutomated.value,
                      event.type == Command::ParamAutomated);
            break;
        case Command::LatencyChanged:
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
            sendUpdate(channel, false);
            break;
        default:
            LOG_ERROR("bug PluginHandle::sendEvents");
            break;
        }
    }
}

void PluginHandle::sendUpdate(ShmChannel& channel, bool bank){
    // compare new param state with cached one
    // and send all parameters that have changed

    channel.clear(); // !

    auto numParams = plugin_->info().numParameters();
    for (int i = 0; i < numParams; ++i){
        auto value = plugin_->getParameter(i);
        if (value != paramState_[i]){
            sendParam(channel, i, value, false);
            paramState_[i] = value;
        }
    }

    auto sendProgramName = [&](int index, const std::string& name){
        auto size = sizeof(ShmCommand) + name.size() + 1;
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
        sendProgramName(plugin_->getProgram(), plugin_->getProgramName());
    }
}

void PluginHandle::sendParam(ShmChannel &channel, int index,
                             float value, bool automated)
{
    std::string display = plugin_->getParameterString(index);

    auto size  = CommandSize(ShmReply, paramState, display.size() + 1);
    auto reply = (ShmReply *)alloca(size);
    reply->type = automated ? Command::ParamAutomated : Command::ParameterUpdate;
    reply->paramState.index = index;
    reply->paramState.value = value;
    memcpy(reply->paramState.display, display.c_str(), display.size() + 1);

    addReply(channel, reply, size);
}

void PluginHandle::addReply(ShmChannel& channel, const void *cmd, size_t size){
    channel.addMessage(static_cast<const char *>(cmd), size);
}

/*////////////////// PluginServer ////////////////*/

static PluginManager gPluginManager;

PluginServer::PluginServer(int pid, const std::string& shmPath)
    : pid_(pid), shm_(std::make_unique<ShmInterface>())
{
    shm_->connect(shmPath);
    LOG_DEBUG("PluginServer: connected to shared memory interface");
    // setup UI event loop
    UIThread::setup();
    LOG_DEBUG("PluginServer: setup event loop");
    // install UI poll function
    pollFunction_ = UIThread::addPollFunction([](void *x){
        static_cast<PluginServer *>(x)->pollUIThread();
    }, this);
    LOG_DEBUG("PluginServer: add UI poll function");
    // create threads
    running_ = true;

    for (int i = 2; i < shm_->numChannels(); ++i){
        auto thread = std::thread(&PluginServer::runThread,
                                  this, &shm_->getChannel(i));
        threads_.push_back(std::move(thread));
    }
    LOG_DEBUG("PluginServer: create threads");
}

PluginServer::~PluginServer(){
    for (auto& thread : threads_){
        thread.join();
    }
}

void PluginServer::run(){
    UIThread::run();
}

void PluginServer::postUIThread(const ShmUICommand& cmd){
    // sizeof(cmd) is a bit lazy, but we don't care about size here
    auto& channel = shm_->getChannel(1);
    channel.writeMessage((const char *)&cmd, sizeof(cmd));
    channel.post();
}

void PluginServer::pollUIThread(){
    // poll events from channel 0 and dispatch to plugin
    auto& channel = shm_->getChannel(0);

    ShmUICommand cmd;
    size_t size = sizeof(cmd);

    while (channel.readMessage((char *)&cmd, size)){
        auto plugin = findPlugin(cmd.id);
        if (plugin){
            plugin->handleUICommand(cmd);
        } else {
            LOG_ERROR("PluginServer::pollUIThread: couldn't find plugin " << cmd.id);
        }
        size = sizeof(cmd); // reset size!
    }
}

void PluginServer::runThread(ShmChannel *channel){
    // while (running_) wait for requests
    // dispatch requests to plugin
    // Quit command -> quit()
    for (;;){
        channel->wait();

        channel->reset();

        const char *msg;
        size_t size;
        if (channel->getMessage(msg, size)){
            handleCommand(*channel, *reinterpret_cast<const ShmCommand *>(msg));
        } else if (!running_) {
            // thread got woken up after quit message
            break;
        } else {
            LOG_ERROR("PluginServer: '" << channel->name()
                      << "': couldn't get message");
            // ?
            channel->postReply();
        }
    }
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
    } catch (const Error& e){
        channel.clear(); // !

        std::string msg = e.what();

        char buf[256]; // can't use alloca() in catch block
        auto size = CommandSize(ShmCommand, error, msg.size() + 1);
        auto reply = new(buf)ShmCommand(Command::Error);
        reply->error.code = e.code();
        memcpy(reply->error.msg, msg.c_str(), msg.size() + 1);

        channel.addMessage((const char *)reply, size);
    }

    channel.postReply();
}

void PluginServer::createPlugin(uint32_t id, const char *data, size_t size,
                                ShmChannel& channel){
    LOG_DEBUG("PluginServer: create plugin " << id);
    std::stringstream ss;
    ss.str(std::string(data, size));

    struct PluginResult {
        PluginInfo::const_ptr info;
        IPlugin::ptr plugin;
        Error error;
    } result;

    result.info = gPluginManager.readPlugin(ss);
    if (!result.info){
        // shouldn't happen...
        throw Error(Error::PluginError, "plugin info out of date!");
    }

    // create on UI thread!
    UIThread::callSync([](void *x){
        auto result = static_cast<PluginResult *>(x);
        try {
            // open with Mode::Native to avoid infinite recursion!
            result->plugin = result->info->create(true, false,
                                                  PluginInfo::Mode::Native);
        } catch (const Error& e){
            result->error = e;
        }
    }, &result);

    if (result.plugin){
        WriteLock lock(pluginMutex_);
        auto handle = PluginHandle(*this, std::move(result.plugin), id, channel);
        plugins_.emplace(id, std::move(handle));
    } else {
        throw result.error;
    }
}

void PluginServer::destroyPlugin(uint32_t id){
    LOG_DEBUG("PluginServer: destroy plugin " << id);
    WriteLock lock(pluginMutex_);
    auto it = plugins_.find(id);
    if (it != plugins_.end()){
        PluginHandle plugin = std::move(it->second);
        plugins_.erase(it);

        lock.unlock();

        // release on UI thread!
        UIThread::callSync([](void *x){
            auto dummy = std::move(*static_cast<PluginHandle *>(x));
        }, &plugin);
    } else {
        LOG_ERROR("PluginServer::destroyPlugin: chouldn't find plugin " << id);
    }
}

PluginHandle * PluginServer::findPlugin(uint32_t id){
    ReadLock lock(pluginMutex_);
    auto it = plugins_.find(id);
    if (it != plugins_.end()){
        return &it->second;
    } else {
        return nullptr;
    }
}

void PluginServer::quit(){
    LOG_DEBUG("PluginServer: quit");
    UIThread::removePollFunction(pollFunction_);
    running_ = false;
    // wake up all threads
    for (int i = 2; i < shm_->numChannels(); ++i){
        shm_->getChannel(i).post();
    }
    // quit event loop
    UIThread::quit();
}

} // vst
