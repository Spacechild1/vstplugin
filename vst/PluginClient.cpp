#include "PluginClient.h"
#include "Utility.h"

#include <algorithm>
#include <sstream>
#include <cassert>

namespace vst {

/*/////////////////////// PluginClient /////////////////////////////*/

#define FORBIDDEN_METHOD(name) throw Error(Error::PluginError, "PluginClient: must not call " name "()");

#define UNSUPPORTED_METHOD(name) LOG_WARNING(name "() not supported with bit bridging/sandboxing");

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

    paramCache_ = std::make_unique<Param[]>(info_->numParameters());
    programCache_ = std::make_unique<std::string[]>(info_->numPrograms());

    LOG_DEBUG("PluginClient: get plugin bridge");
    if (sandbox){
        bridge_ = PluginBridge::create(factory_->arch());
    } else {
        bridge_ = PluginBridge::getShared(factory_->arch());
    }

    // create plugin
    std::stringstream ss;
    info_->serialize(ss);
    auto info = ss.str();
    LOG_DEBUG("PluginClient: open plugin (info size: "
              << info.size() << ")");

    auto cmdSize = sizeof(ShmCommand) + info.size();
    auto data = std::make_unique<char[]>(cmdSize);
    auto cmd = new (data.get()) ShmCommand(Command::CreatePlugin, id());
    cmd->plugin.size = info.size();
    memcpy(cmd->plugin.data, info.c_str(), info.size());

    auto chn = bridge_->getNRTChannel();
    if (chn.addCommand(cmd, cmdSize)){
        chn.send();
    } else {
        // info too large, try transmit via tmp file
        std::stringstream ss;
        ss << getTmpDirectory() << "/vst_" << (void *)this;
        std::string path = ss.str();
        TmpFile file(path, File::WRITE);

        file << info;

        cmdSize = sizeof(ShmCommand) + path.size() + 1;
        cmd->plugin.size = 0; // !
        memcpy(cmd->plugin.data, path.c_str(), path.size() + 1);

        if (!chn.addCommand(cmd, cmdSize)){
            throw Error(Error::PluginError,
                        "PluginClient: couldn't send plugin info");
        }

        chn.send(); // tmp file is still in scope!
    }

    // in case the process has already crashed during creation...
    if (!bridge_->alive()){
        throw Error(Error::PluginError, "plugin crashed");
    }

    // collect replies (after check!)
    const ShmCommand *reply;
    while (chn.getReply(reply)){
        dispatchReply(*reply);
    }

    LOG_DEBUG("PluginClient: done!");
}

PluginClient::~PluginClient(){
    if (!listener_.expired()){
        bridge_->removeUIClient(id_);
    }
    // destroy plugin
    // (not necessary with exlusive bridge)
    if (bridge_->shared() && bridge_->alive()){
        ShmCommand cmd(Command::DestroyPlugin, id());

        auto chn = bridge_->getNRTChannel();
        chn.AddCommand(cmd, empty);
        chn.send();
    }

    // avoid memleak with param string and sysex command
    for (auto& cmd : commands_){
        if (cmd.type == Command::SetParamString){
            delete[] cmd.paramString.display;
        } else if (cmd.type == Command::SendSysex){
            delete[] cmd.sysex.data;
        }
    }
    LOG_DEBUG("free PluginClient");
}

bool PluginClient::check() {
    return bridge_->alive();
}

void PluginClient::setupProcessing(double sampleRate, int maxBlockSize,
                                   ProcessPrecision precision){
    if (!check()){
        return;
    }
    LOG_DEBUG("PluginClient: setupProcessing");

    ShmCommand cmd(Command::SetupProcessing);
    cmd.id = id();
    cmd.setup.sampleRate = sampleRate;
    cmd.setup.maxBlockSize = maxBlockSize;
    cmd.setup.precision = static_cast<uint32_t>(precision);

    auto chn = bridge().getNRTChannel();
    chn.AddCommand(cmd, setup);
    chn.send();

    chn.checkError();
}

template<typename T>
void PluginClient::doProcess(ProcessData& data){
    if (!check()){
        bypass(data);
        commands_.clear(); // avoid commands piling up!
        return;
    }
    // LOG_DEBUG("PluginClient: process");

    auto channel = bridge().getRTChannel();

    // LOG_DEBUG("PluginClient: send process command");
    // send process command
    ShmCommand cmd(Command::Process);
    cmd.id = id();
    cmd.process.numSamples = data.numSamples;
    cmd.process.precision = (uint16_t)data.precision;
    cmd.process.numInputs = data.numInputs;
    cmd.process.numOutputs = data.numOutputs;

    channel.AddCommand(cmd, process);

    // send input busses
    for (int i = 0; i < data.numInputs; ++i){
        auto& bus = data.inputs[i];
        // write all channels sequentially to avoid additional copying.
        for (int j = 0; j < bus.numChannels; ++j){
            channel.addCommand((const T *)bus.channelData32[j], sizeof(T) * data.numSamples);
        }
    }

    // add commands (parameter changes, MIDI messages, etc.)
    // LOG_DEBUG("PluginClient: send commands");
    sendCommands(channel);

    // send and wait for reply
    // LOG_DEBUG("PluginClient: wait");
    channel.send();

    // check if host is still alive
    if (!check()){
        bypass(data);
        commands_.clear(); // avoid commands piling up!
        return;
    }

    // read output busses
    for (int i = 0; i < data.numOutputs; ++i){
        auto& bus = data.outputs[i];
        // LOG_DEBUG("PluginClient: read audio bus " << i << " with "
        //     << bus.numChannels << " channels");
        // read channels
        for (int j = 0; j < bus.numChannels; ++j){
            auto chn = (T *)bus.channelData32[j];
            const T* reply;
            size_t size;
            if (channel.getReply(reply, size)){
                // size can be larger because of message
                // alignment - don't use in std::copy!
                assert(size >= data.numSamples * sizeof(T));
                std::copy(reply, reply + data.numSamples, chn);
            } else {
                std::fill(chn, chn + data.numSamples, 0);
                LOG_ERROR("PluginClient: missing channel " << j
                          << " for audio output bus " << i);
            }
        }
    }

    // get replies (parameter changes, MIDI messages, etc.)
    // LOG_DEBUG("PluginClient: read replies");
    const ShmCommand* reply;
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
        case Command::SetParamString:
        {
            auto displayLen = strlen(cmd.paramString.display) + 1;
            auto cmdSize = CommandSize(ShmCommand, paramString, displayLen);
            auto shmCmd = (ShmCommand *)alloca(cmdSize);
            new (shmCmd) ShmCommand(Command::SetParamString);
            shmCmd->paramString.index = cmd.paramString.index;
            shmCmd->paramString.offset = cmd.paramString.offset;
            memcpy(shmCmd->paramString.display,
                   cmd.paramString.display, displayLen);

            delete[] cmd.paramString.display; // free!

            channel.addCommand(shmCmd, cmdSize);
            break;
        }
        case Command::SetProgramName:
        {
            auto len = strlen(cmd.s) + 1;
            auto cmdSize = CommandSize(ShmCommand, s, len);
            auto shmCmd = (ShmCommand *)alloca(cmdSize);
            new (shmCmd) ShmCommand(Command::SetProgramName);
            memcpy(shmCmd->s, cmd.s, len);

            delete[] cmd.s; // free!

            channel.addCommand(shmCmd, cmdSize);
            break;
        }
        case Command::SendMidi:
            channel.AddCommand(cmd, midi);
            break;
        case Command::SendSysex:
        {
            auto cmdSize = CommandSize(ShmCommand, sysex, cmd.sysex.size);
            auto shmCmd = (ShmCommand *)alloca(cmdSize);
            new (shmCmd) ShmCommand(Command::SendSysex);
            shmCmd->sysex.delta = cmd.sysex.delta;
            shmCmd->sysex.size = cmd.sysex.size;
            memcpy(shmCmd->sysex.data, cmd.sysex.data, cmd.sysex.size);

            delete[] cmd.sysex.data; // free!

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

    commands_.clear(); // !
}

void PluginClient::dispatchReply(const ShmCommand& reply){
    // LOG_DEBUG("PluginClient: got reply " << reply.type);
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
            LOG_DEBUG("PluginClient: parameter " << index
                      << " automated ");
        } else {
            LOG_DEBUG("PluginClient: parameter " << index
                      << " updated to " << value);
        }
        break;
    }
    case Command::ProgramNameIndexed:
        if (info().numPrograms() > 0){
            programCache_[reply.programName.index]
                    = reply.programName.name;
        }
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
            SysexEvent sysex;
            sysex.delta = reply.sysex.delta;
            sysex.size = reply.sysex.size;
            sysex.data = reply.sysex.data;

            listener->sysexEvent(sysex);
        }
        break;
    }
    case Command::Error:
        reply.throwError();
        break;
    default:
        LOG_ERROR("got unknown reply " << reply.type);
        break;
    }
}

void PluginClient::process(ProcessData& data){
    if (data.precision == ProcessPrecision::Double){
        doProcess<double>(data);
    } else {
        doProcess<float>(data);
    }
}

void PluginClient::suspend(){
    if (!check()){
        return;
    }
    LOG_DEBUG("PluginClient: suspend");
    ShmCommand cmd(Command::Suspend, id());

    auto chn = bridge().getNRTChannel();
    chn.AddCommand(cmd, empty);
    chn.send();

    chn.checkError();
}

void PluginClient::resume(){
    if (!check()){
        return;
    }

    LOG_DEBUG("PluginClient: resume");
    ShmCommand cmd(Command::Resume, id());

    auto chn = bridge().getNRTChannel();
    chn.AddCommand(cmd, empty);
    chn.send();

    chn.checkError();
}

void PluginClient::setNumSpeakers(int *input, int numInputs,
                                  int *output, int numOutputs){
    if (!check()){
        return;
    }

    LOG_DEBUG("requested bus arrangement:");
    for (int i = 0; i < numInputs; ++i){
        LOG_DEBUG("input bus " << i << ": " << input[i] << "ch");
    }
    for (int i = 0; i < numOutputs; ++i){
        LOG_DEBUG("output bus " << i << ": " << output[i] << "ch");
    }

    int size = sizeof(int32_t) * (numInputs + numOutputs);
    auto totalSize = CommandSize(ShmCommand, speakers, size);
    auto cmd = (ShmCommand *)alloca(totalSize);
    new (cmd) ShmCommand(Command::SetNumSpeakers, id());
    cmd->speakers.numInputs = numInputs;
    cmd->speakers.numOutputs = numOutputs;
    // copy input and output arrangements
    for (int i = 0; i < numInputs; ++i){
        cmd->speakers.speakers[i] = input[i];
    }
    for (int i = 0; i < numOutputs; ++i){
        cmd->speakers.speakers[i + numInputs] = output[i];
    }

    auto chn = bridge().getNRTChannel();
    chn.addCommand(cmd, totalSize);
    chn.send();

    // check if host is still alive!
    if (!check()){
        return;
    }

    // get reply
    const ShmCommand* reply;
    if (chn.getReply(reply)){
        if (reply->type == Command::SpeakerArrangement){
            // get actual input and output arrangements
            assert(reply->speakers.numInputs == numInputs);
            assert(reply->speakers.numOutputs == numOutputs);
            for (int i = 0; i < numInputs; ++i){
                input[i] = reply->speakers.speakers[i];
            }
            for (int i = 0; i < numOutputs; ++i){
                output[i] = reply->speakers.speakers[i + numInputs];
            }
        } else if (reply->type == Command::Error){
            reply->throwError();
        } else {
            LOG_ERROR("setNumSpeakers: unknown reply");
        }
    } else {
        LOG_ERROR("setNumSpeakers: missing reply!");
    }

    LOG_DEBUG("actual bus arrangement:");
    for (int i = 0; i < numInputs; ++i){
        LOG_DEBUG("input bus " << i << ": " << input[i] << "ch");
    }
    for (int i = 0; i < numOutputs; ++i){
        LOG_DEBUG("output bus " << i << ": " << output[i] << "ch");
    }
}

int PluginClient::getLatencySamples(){
    return latency_;
}

void PluginClient::setListener(IPluginListener::ptr listener) {
    listener_ = listener;
    if (listener){
        bridge_->addUIClient(id_, listener);
    } else {
        bridge_->removeUIClient(id_);
    }
}

double PluginClient::getTransportPosition() const {
    // TODO
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

    Command cmd(Command::SetProgram);
    cmd.s = new char[name.size() + 1];
    memcpy(cmd.s, name.c_str(), name.size() + 1);

    commands_.push_back(cmd);
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
    if (!check()){
        return;
    }

    auto totalSize = CommandSize(ShmCommand, buffer, size);
    auto cmd = (ShmCommand *)alloca(totalSize);
    new (cmd) ShmCommand(type, id());
    cmd->buffer.size = size;
    memcpy(cmd->buffer.data, data, size);

    auto chn = bridge().getNRTChannel();
    chn.addCommand(cmd, totalSize);
    chn.send();

    // check if host is still alive!
    if (!check()){
        return;
    }

    // get replies
    const ShmCommand* reply;
    while (chn.getReply(reply)){
        dispatchReply(*reply);
    }
}

void PluginClient::receiveData(Command::Type type, std::string &buffer){
    if (!check()){
        return;
    }

    ShmCommand cmd(type, id());

    auto chn = bridge().getNRTChannel();
    chn.AddCommand(cmd, empty);
    chn.send();

    // check if host is still alive!
    if (!check()){
        return;
    }

    const ShmCommand *reply;
    if (chn.getReply(reply)){
        if (reply->type == Command::PluginData){
            auto len = reply->i;
            // data is in a seperate message!
            const char *data;
            size_t size;
            if (chn.getReply(data, size)){
                assert(size >= len); // 'size' can be larger because of padding!
                buffer.assign(data, len);
            } else {
                throw Error(Error::PluginError,
                            "PluginClient::receiveData: missing data message");
            }
        } else if (reply->type == Command::Error){
           reply->throwError();
        } else {
            throw Error(Error::PluginError,
                        "PluginClient::receiveData: unexpected reply message");
        }
    } else {
        throw Error(Error::PluginError,
                    "PluginClient::receiveData: missing reply message");
    }
}

void PluginClient::openEditor(void * window){
    FORBIDDEN_METHOD("openEditor")
}

void PluginClient::closeEditor(){
    FORBIDDEN_METHOD("closeEditor")
}

bool PluginClient::getEditorRect(Rect& rect) const {
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
    UNSUPPORTED_METHOD("beginMessage");
}

void PluginClient::addInt(const char* id, int64_t value){
    UNSUPPORTED_METHOD("addInt");
}

void PluginClient::addFloat(const char* id, double value){
    UNSUPPORTED_METHOD("addFloat");
}

void PluginClient::addString(const char* id, const char *value){
    UNSUPPORTED_METHOD("addString");
}

void PluginClient::addString(const char* id, const std::string& value){
    UNSUPPORTED_METHOD("addString");
}

void PluginClient::addBinary(const char* id, const char *data, size_t size){
    UNSUPPORTED_METHOD("addBinary");
}

void PluginClient::endMessage(){
    UNSUPPORTED_METHOD("endMessage");
}


/*///////////////////// WindowClient ////////////////////*/

WindowClient::WindowClient(PluginClient& plugin)
    : plugin_(&plugin) {}

WindowClient::~WindowClient(){}

void* WindowClient::getHandle() {
    return nullptr;
}

void WindowClient::open(){
    LOG_DEBUG("WindowOpen");
    ShmUICommand cmd(Command::WindowOpen, plugin_->id());
    plugin_->bridge().postUIThread(cmd);
}

void WindowClient::close(){
    LOG_DEBUG("WindowClose");
    ShmUICommand cmd(Command::WindowClose, plugin_->id());
    plugin_->bridge().postUIThread(cmd);
}

void WindowClient::setPos(int x, int y){
    LOG_DEBUG("WindowSetPos");
    ShmUICommand cmd(Command::WindowSetPos, plugin_->id());
    cmd.windowPos.x = x;
    cmd.windowPos.y = y;
    plugin_->bridge().postUIThread(cmd);
}

void WindowClient::setSize(int w, int h){
    LOG_DEBUG("WindowSetSize");
    ShmUICommand cmd(Command::WindowSetSize, plugin_->id());
    cmd.windowSize.width = w;
    cmd.windowSize.height = h;
    plugin_->bridge().postUIThread(cmd);
}

void WindowClient::update(){
    // ignore
}

} // vst
