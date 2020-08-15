#include "PluginClient.h"
#include "Utility.h"

#include <algorithm>

namespace vst {

/*/////////////////////// PluginClient /////////////////////////////*/

#define FORBIDDEN_METHOD(name) throw Error(Error::PluginError, "PluginClient: must not call " name "()");

#define UNSUPPORTED_METHOD(name) LOG_WARNING(name "() not supported with bit bridging");

#define ShmRTCommandAlloca(type, extra) new(alloca(sizeof(ShmRTCommand) + extra))ShmRTCommand(type)

#define ShmNRTCommandAlloca(type, id, extra) new(alloca(sizeof(ShmNRTCommand) + extra))ShmNRTCommand(type, id)

IPlugin::ptr makeBridgedPlugin(IFactory::const_ptr factory, const std::string& name,
                               bool editor, bool sandbox)
{
    auto info = factory->findPlugin(name); // should never fail
    if (!info){
        throw Error(Error::PluginError, "couldn't find subplugin");
    }
    auto plugin = std::make_unique<PluginClient>(factory, info, sandbox);
    if (editor){
        auto window = std::make_unique<WindowClient>(*plugin);
        plugin->setWindow(std::move(window));
    }
    return std::move(plugin); // Clang bug
}

PluginClient::PluginClient(IFactory::const_ptr f, PluginInfo::const_ptr desc, bool sandbox)
    : factory_(std::move(f)), info_(std::move(desc))
{
    static std::atomic<uint32_t> nextID{0};
    id_ = ++nextID; // atomic increment!

    paramCache_.reset(new Param[info_->numParameters()]);
    programCache_.reset(new std::string[info_->numPrograms()]);

    if (sandbox){
        bridge_ = PluginBridge::create(f->arch());
    } else {
        bridge_ = PluginBridge::getShared(f->arch());
    }

    // create plugin
    auto bufsize = info_->path().size() + info_->name.size() + 2; // include `\0` bytes
    auto cmdSize = sizeof(ShmCommand) + bufsize;
    auto cmd = (ShmCommand *)alloca(cmdSize);
    cmd->type = Command::CreatePlugin;
    cmd->id = id_;
    cmd->buffer.size = bufsize;
    snprintf(cmd->buffer.data, bufsize, "%s\0%s", info_->path().c_str(), info_->name.c_str());

    auto chn = bridge_->getNRTChannel();
    chn.addCommand(cmd, cmdSize);
    chn.send();
}

PluginClient::~PluginClient(){
    bridge_->removeUIClient(id_);
    // destroy plugin
    // (not necessary with exlusive bridge)
    if (bridge_->shared()){
        ShmCommand cmd(Command::DestroyPlugin);
        cmd.id = id_;

        auto chn = bridge_->getNRTChannel();
        chn.AddCommand(cmd, id);
        chn.send();
    }

    // avoid memleak with param string and sysex command
    for (auto& cmd : commands_){
        if (cmd.type == Command::SetParamString){
            delete cmd.paramString.display;
        } else if (cmd.type == Command::SendSysex){
            delete cmd.sysex.data;
        }
    }
}

void PluginClient::setupProcessing(double sampleRate, int maxBlockSize, ProcessPrecision precision){
    ShmCommand cmd(Command::SetupProcessing);
    cmd.id = id_;
    cmd.setup.sampleRate = sampleRate;
    cmd.setup.maxBlockSize = maxBlockSize;
    cmd.setup.precision = static_cast<uint32_t>(precision);

    auto chn = bridge().getNRTChannel();
    chn.AddCommand(cmd, setup);
    chn.send();
}

template<typename T>
void PluginClient::doProcess(ProcessData<T>& data){
    auto channel = bridge().getRTChannel();

    // send process command
    {
        ShmCommand cmd(Command::Process);
        cmd.id = id();
        cmd.process.numInputs = data.numInputs;
        cmd.process.numOutputs = data.numOutputs;
        cmd.process.numAuxInputs = data.numAuxInputs;
        cmd.process.numAuxOutpus = data.numAuxOutputs;
        cmd.process.numSamples = data.numSamples;

        channel.AddCommand(cmd, process);
    }

    // write audio data
    // since we have sent the number of channels in the "Process" command,
    // we can simply write all channels sequentially to avoid additional copying.
    auto writeBus = [&](const T** bus, int numChannels){
        for (int i = 0; i < numChannels; ++i){
            channel.addCommand(bus[i], sizeof(T) * data.numSamples);
        }
    };

    writeBus(data.input, data.numInputs);
    writeBus(data.auxInput, data.numAuxInputs);

    // add commands (parameter changes, MIDI messages, etc.)
    sendCommands(channel);

    // send and wait for reply
    channel.send();

    // read audio data
    // here we simply read all channels sequentially.
    auto readBus = [&](T** bus, int numChannels){
        for (int i = 0; i < numChannels; ++i){
            const T* chn;
            if (channel.getReply(chn)){
                std::copy(chn, chn + data.numSamples, bus[i]);
            }
        }
    };

    readBus(data.output, data.numOutputs);
    readBus(data.auxOutput, data.numAuxOutputs);

    // get replies (parameter changes, MIDI messages, etc.)
    const ShmReply* reply;
    while (channel.getReply(reply)){
        dispatchReply(*reply);
    }
}

void PluginClient::sendCommands(RTChannel& channel){
    for (auto& cmd : commands_){
        // we have to handle some commands specially:
        switch (cmd.type){
        case Command::SetParamValue:
            channel.AddCommand(cmd, paramValue);
            break;
        case Command::SendMidi:
            channel.AddCommand(cmd, midi);
            break;
        case Command::SetTimeSignature:
            channel.AddCommand(cmd, timeSig);
            break;
        case Command::SetParamString:
        {
            auto displayLen = strlen(cmd.paramString.display) + 1;
            auto cmdSize = CommandSize(ShmCommand, paramString, displayLen);
            auto shmCmd = (ShmCommand *)alloca(cmdSize);
            shmCmd->type = Command::SetParamString;
            shmCmd->paramString.index = cmd.paramString.index;
            shmCmd->paramString.offset = cmd.paramString.offset;
            memcpy(shmCmd->paramString.display,
                   cmd.paramString.display, displayLen);

            delete cmd.paramString.display; // free!

            channel.addCommand(shmCmd, cmdSize);
            break;
        }
        case Command::SendSysex:
        {
            auto cmdSize = CommandSize(ShmCommand, sysex, cmd.sysex.size);
            auto shmCmd = (ShmCommand *)alloca(cmdSize);
            shmCmd->type = Command::SetParamString;
            shmCmd->sysex.delta = cmd.sysex.delta;
            shmCmd->sysex.size = cmd.sysex.size;
            memcpy(shmCmd->sysex.data, cmd.sysex.data, cmd.sysex.size);

            delete cmd.sysex.data; // free!

            channel.addCommand(shmCmd, cmdSize);
            break;
        }
        // all other commands take max. 8 bytes and they are
        // rare enough that we don't have to optimize for space
        default:
            channel.AddCommand(cmd, d);
            break;
        }
    }
}

void PluginClient::dispatchReply(const ShmReply& reply){
    switch (reply.type){
    case Command::ParamAutomated:
    case Command::ParameterUpdate:
    {
        auto index = reply.paramState.index;
        auto value = reply.paramState.value;
        paramCache_[index].value = value;
        paramCache_[index].display = reply.paramState.display;

        if (reply.type == Command::ParamAutomated){
            auto listener = listener_.lock();
            if (listener){
                listener->parameterAutomated(index, value);
            }
        }
        break;
    }
    case Command::ProgramName:
        programCache_[program_] = reply.s;
        break;
    case Command::ProgramNumber:
        program_ = reply.i;
        break;
    case Command::LatencyChanged:
    {
        latency_ = reply.i;
        auto listener = listener_.lock();
        if (listener){
            listener->latencyChanged(latency_);
        }
        break;
    }
    case Command::MidiReceived:
    {
        auto listener = listener_.lock();
        if (listener){
            listener->midiEvent(reply.midi);
        }
        break;
    }
    case Command::SysexReceived:
    {
        auto listener = listener_.lock();
        if (listener){
            // put temporary copy on the stack
            auto data = (char *)alloca(reply.sysex.size);
            memcpy(data, reply.sysex.data, reply.sysex.size);

            SysexEvent sysex;
            sysex.delta = reply.sysex.delta;
            sysex.size = reply.sysex.size;
            sysex.data = data;

            listener->sysexEvent(sysex);
        }
        break;
    }
    default:
        LOG_ERROR("got unknown reply " << reply.type);
        break;
    }
}

void PluginClient::process(ProcessData<float>& data){
    doProcess(data);
}

void PluginClient::process(ProcessData<double>& data){
    doProcess(data);
}

void PluginClient::suspend(){
    ShmCommand cmd(Command::Suspend);
    cmd.id = id();

    auto chn = bridge().getNRTChannel();
    chn.AddCommand(cmd, empty);
    chn.send();
}

void PluginClient::resume(){
    ShmCommand cmd(Command::Resume);
    cmd.id = id();

    auto chn = bridge().getNRTChannel();
    chn.AddCommand(cmd, empty);
    chn.send();
}

void PluginClient::setNumSpeakers(int in, int out, int auxin, int auxout){
    ShmCommand cmd(Command::SetNumSpeakers);
    cmd.id = id();
    cmd.speakers.in = in;
    cmd.speakers.auxin = auxin;
    cmd.speakers.out = out;
    cmd.speakers.auxout = auxout;

    auto chn = bridge().getNRTChannel();
    chn.AddCommand(cmd, speakers);
    chn.send();
}

int PluginClient::getLatencySamples(){
    return latency_;
}

void PluginClient::setListener(IPluginListener::ptr listener) {
    listener_ = listener;
    bridge_->addUIClient(id_, listener);
}

double PluginClient::getTransportPosition() const {
    return transport_;
}

void PluginClient::setParameter(int index, float value, int sampleOffset){
    paramCache_[index].value = value; // cache value
    DeferredPlugin::setParameter(index, value, sampleOffset);
}

bool PluginClient::setParameter(int index, const std::string &str, int sampleOffset){
    paramCache_[index].display = str; // cache display
    return DeferredPlugin::setParameter(index, str, sampleOffset);
}

float PluginClient::getParameter(int index) const {
    return paramCache_[index].value;
}

std::string PluginClient::getParameterString(int index) const {
    return paramCache_[index].display;
}

void PluginClient::setProgramName(const std::string& name){
    programCache_[program_] = name;
    // TODO send to plugin
}

int PluginClient::getProgram() const {
    return program_;
}

std::string PluginClient::getProgramName() const {
    return programCache_[program_];
}

std::string PluginClient::getProgramNameIndexed(int index) const {
    return programCache_[index];
}

void PluginClient::readProgramFile(const std::string& path){
    sendData(Command::ReadProgramFile, path.c_str(), path.size() + 1);
}

void PluginClient::readProgramData(const char *data, size_t size){
    sendData(Command::ReadProgramData, data, size);
}

void PluginClient::readBankFile(const std::string& path){
    sendData(Command::ReadBankFile, path.c_str(), path.size() + 1);
}

void PluginClient::readBankData(const char *data, size_t size){
    sendData(Command::ReadBankData, data, size);
}

void PluginClient::writeProgramFile(const std::string& path){
    sendData(Command::WriteProgramFile, path.c_str(), path.size() + 1);
}

void PluginClient::writeProgramData(std::string& buffer){
    receiveData(Command::WriteProgramData, buffer);
}

void PluginClient::writeBankFile(const std::string& path){
    sendData(Command::WriteBankFile, path.c_str(), path.size() + 1);
}

void PluginClient::writeBankData(std::string& buffer){
    receiveData(Command::WriteBankData, buffer);
}

void PluginClient::sendData(Command::Type type, const char *data, size_t size){
    auto totalSize = CommandSize(ShmCommand, buffer, size);
    auto cmd = (ShmCommand *)alloca(totalSize);
    cmd->type = type;
    cmd->id = id();
    cmd->buffer.size = size;
    memcpy(cmd->buffer.data, data, size);

    auto chn = bridge().getNRTChannel();
    chn.addCommand(cmd, totalSize);
    chn.send();

    // get replies
    const ShmReply* reply;
    while (chn.getReply(reply)){
        dispatchReply(*reply);
    }
}

void PluginClient::receiveData(Command::Type type, std::string &buffer){
    ShmCommand cmd(type);
    cmd.id = id();

    auto chn = bridge().getNRTChannel();
    chn.AddCommand(cmd, empty);
    chn.send();

    const ShmReply *reply;
    if (chn.getReply(reply) && reply->type == Command::PluginData){
        buffer.assign(reply->buffer.data, reply->buffer.size);
    }
}

void PluginClient::openEditor(void * window){
    FORBIDDEN_METHOD("openEditor")
}

void PluginClient::closeEditor(){
    FORBIDDEN_METHOD("closeEditor")
}

bool PluginClient::getEditorRect(int &left, int &top, int &right, int &bottom) const {
    FORBIDDEN_METHOD("getEditorRect")
}

void PluginClient::updateEditor() {
    FORBIDDEN_METHOD("updateEditor")
}

void PluginClient::checkEditorSize(int& width, int& height) const {
    FORBIDDEN_METHOD("checkEditorSize")
}

void PluginClient::resizeEditor(int width, int height) {
    FORBIDDEN_METHOD("resizeEditor")
}

bool PluginClient::canResize() const {
    FORBIDDEN_METHOD("canResize")
}

// VST2 only

int PluginClient::canDo(const char *what) const {
    UNSUPPORTED_METHOD("canDo")
    return 0;
}

intptr_t PluginClient::vendorSpecific(int index, intptr_t value, void *p, float opt){
    UNSUPPORTED_METHOD("vendorSpecific");
    return 0;
}

// VST3 only

void PluginClient::beginMessage(){

}

void PluginClient::addInt(const char* id, int64_t value){

}

void PluginClient::addFloat(const char* id, double value){

}

void PluginClient::addString(const char* id, const char *value){

}

void PluginClient::addString(const char* id, const std::string& value){

}

void PluginClient::addBinary(const char* id, const char *data, size_t size){

}

void PluginClient::endMessage(){

}


/*///////////////////// WindowClient ////////////////////*/

WindowClient::WindowClient(PluginClient& plugin)
    : plugin_(&plugin) {}

WindowClient::~WindowClient(){}

void* WindowClient::getHandle() {
    return nullptr;
}

void WindowClient::open(){
    ShmUICommand cmd(Command::WindowOpen, plugin_->id());
    plugin_->bridge().postUIThread(cmd);
}

void WindowClient::close(){
    ShmUICommand cmd(Command::WindowClose, plugin_->id());
    plugin_->bridge().postUIThread(cmd);
}

void WindowClient::setPos(int x, int y){
    ShmUICommand cmd(Command::WindowSetPos, plugin_->id());
    cmd.windowPos.x = x;
    cmd.windowPos.y = y;
    plugin_->bridge().postUIThread(cmd);
}

void WindowClient::setSize(int w, int h){
    ShmUICommand cmd(Command::WindowSetSize, plugin_->id());
    cmd.windowSize.width = w;
    cmd.windowSize.height = h;
    plugin_->bridge().postUIThread(cmd);
}

void WindowClient::update(){
    // ignore
}

} // vst
