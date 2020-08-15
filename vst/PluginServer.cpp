#include "PluginServer.h"
#include "ShmInterface.h"
#include "Utility.h"

#include <cstring>

namespace vst {

/*///////////////// PluginHandle ///////////////*/

PluginHandle::PluginHandle(PluginServer& server, IPlugin::ptr plugin, uint32_t id)
    : server_(&server), plugin_(std::move(plugin)), id_(id)
{
    auto nparams = plugin_->info().numParameters();
    paramState_.reset(new ParamState[nparams]);
}

void PluginHandle::handleRequest(const ShmCommand &cmd,
                                 ShmChannel &channel)
{
    switch (cmd.type){
    case Command::Process:
        process(cmd, channel);
        break;
    case Command::SetupProcessing:
        plugin_->setupProcessing(cmd.setup.sampleRate,
                                 cmd.setup.maxBlockSize,
                                 static_cast<ProcessPrecision>(cmd.setup.precision));
        break;
    case Command::SetNumSpeakers:
        plugin_->setNumSpeakers(cmd.speakers.in, cmd.speakers.out,
                                cmd.speakers.auxin, cmd.speakers.auxout);
        break;
    case Command::Suspend:
        plugin_->suspend();
        break;
    case Command::Resume:
        plugin_->resume();
        break;
    case Command::ReadProgramFile:
        cacheParamState();
        plugin_->readProgramFile(cmd.buffer.data);
        sendUpdate(channel, false);
        break;
    case Command::ReadBankFile:
        cacheParamState();
        plugin_->readBankFile(cmd.buffer.data);
        sendUpdate(channel, false);
        break;
    case Command::ReadProgramData:
        cacheParamState();
        plugin_->readProgramData(cmd.buffer.data, cmd.buffer.size);
        sendUpdate(channel, true);
        break;
    case Command::ReadBankData:
        cacheParamState();
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
        reply->type = Command::PluginData;
        reply->buffer.id = id_;
        reply->buffer.size = buffer.size();
        memcpy(reply->buffer.data, buffer.data(), buffer.size());

        addReply(channel, reply, size);
        break;
    }
    default:
        LOG_ERROR("PluginHandle::handleRequest: unknown request");
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
            LOG_ERROR("PluginHandle::handleUICommand: unknown command!");
            break;
        }
    } else {
        LOG_ERROR("PluginHandle::handleUICommand: no window!");
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

        commands_.push_back(cmd);
    }
}

void PluginHandle::latencyChanged(int nsamples) {
    if (UIThread::isCurrentThread()){
        // shouldn't happen
    } else {
        Command cmd(Command::LatencyChanged);
        cmd.i = nsamples;

        commands_.push_back(cmd);
    }
}

void PluginHandle::midiEvent(const MidiEvent& event) {
    if (UIThread::isCurrentThread()){
        // ignore for now
    } else {
        Command cmd(Command::MidiReceived);
        cmd.midi = event;

        commands_.push_back(cmd);
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

        commands_.push_back(cmd);
    }
}

void PluginHandle::process(const ShmCommand &cmd, ShmChannel &channel){

}

template<typename T>
void PluginHandle::doProcess(IPlugin::ProcessData<T>& data){

}

void PluginHandle::cacheParamState(){
    // cache param state
}

void PluginHandle::sendUpdate(ShmChannel& channel, bool bank){
    // compare new param state with cached one
    // and send all parameters that have changed
}

void PluginHandle::addParam(ShmChannel &channel, int index,
                            const ParamState &state, bool automated)
{
    auto size  = CommandSize(ShmReply, paramState, state.display.size() + 1);
    auto reply = (ShmReply *)alloca(size);
    reply->type = automated ? Command::ParamAutomated : Command::ParameterUpdate;
    reply->paramState.index = index;
    reply->paramState.value = state.value;
    memcpy(reply->paramState.display, state.display.c_str(), state.display.size() + 1);

    addReply(channel, reply, size);
}

void PluginHandle::addReply(ShmChannel& channel, const void *cmd, size_t size){
    channel.addMessage(static_cast<const char *>(cmd), size);
}

/*////////////////// PluginServer ////////////////*/

PluginServer::PluginServer(int pid, const std::string& shmPath)
    : pid_(pid), shm_(std::make_unique<ShmInterface>())
{
    shm_->connect(shmPath);

    // setup UI event loop
    UIThread::setup();

    // install UI poll function
    UIThread::addPollFunction([](void *x){
        static_cast<PluginServer *>(x)->pollUIThread();
    }, this);

    // create threads
    running_ = true;

    for (int i = 2; i < shm_->numChannels(); ++i){
        auto thread = std::thread(&PluginServer::runThread,
                                  this, &shm_->getChannel(i));
        threads_.push_back(std::move(thread));
    }
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
    while (running_){
        channel->wait();

        const char *msg;
        size_t size;
        PluginHandle *plugin;

        if (channel->getMessage(msg, size)){
            auto cmd = (const ShmCommand *)msg;
            switch (cmd->type){
            case Command::CreatePlugin:
            {
                // TODO
                break;
            }
            case Command::DestroyPlugin:
            {
                // TODO
            }
            case Command::Quit:
                quit();
                break;
            default:
                plugin = findPlugin(cmd->id);
                if (plugin){
                    plugin->handleRequest(*cmd, *channel);
                }
                break;
            }
            channel->postReply();
        } else {
            LOG_ERROR("PluginServer: couldn't get message");
            // ?
        }
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
    running_ = false;
    // wake up all threads
    for (int i = 2; i < shm_->numChannels(); ++i){
        shm_->getChannel(i).post();
    }
    // quit event loop
    UIThread::quit();
}

} // vst
