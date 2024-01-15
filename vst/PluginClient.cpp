#include "PluginClient.h"

#include "Log.h"
#include "MiscUtils.h"
#include "FileUtils.h"

#include <algorithm>
#include <sstream>
#include <cassert>

#if DEBUG_CLIENT_PROCESS
# define LOG_PROCESS(x) LOG_DEBUG(x)
#else
# define LOG_PROCESS(x)
#endif

namespace vst {

/*/////////////////////// PluginClient /////////////////////////////*/

#define FORBIDDEN_METHOD(name) throw Error(Error::PluginError, "PluginClient: must not call " name "()");

#define UNSUPPORTED_METHOD(name) LOG_WARNING(name "() not supported with bit bridging/sandboxing");

IPlugin::ptr createBridgedPlugin(IFactory::const_ptr factory, const std::string& name,
                                 bool editor, bool sandbox)
{
    auto info = factory->findPlugin(name); // should never fail
    if (!info){
        throw Error(Error::PluginError, "couldn't find subplugin");
    }
    return std::make_unique<PluginClient>(factory, info, sandbox, editor);
}

PluginClient::PluginClient(IFactory::const_ptr f, PluginDesc::const_ptr desc,
                           bool sandbox, bool editor)
    : factory_(std::move(f)), info_(std::move(desc))
{
    static std::atomic<uint32_t> nextID{0};
    id_ = ++nextID; // atomic increment!

    if (info_->numParameters() > 0){
        paramCache_ = std::make_unique<Param[]>(info_->numParameters());
    }
    if (info_->numPrograms() > 0){
        programCache_ = std::make_unique<std::string[]>(info_->numPrograms());
    }
    LOG_DEBUG("PluginClient (" << id_ << "): get plugin bridge");
    if (sandbox){
        bridge_ = PluginBridge::create(factory_->arch());
    } else {
        bridge_ = PluginBridge::getShared(factory_->arch());
    }

    // create plugin
    std::stringstream ss;
    info_->serialize(ss);
    auto info = ss.str();
    LOG_DEBUG("PluginClient (" << id_ << "): open plugin (info size: "
              << info.size() << ")");

    auto cmdSize = sizeof(ShmCommand) + info.size();
    auto data = std::make_unique<char[]>(cmdSize);
    auto cmd = new (data.get()) ShmCommand(Command::CreatePlugin, id());
    cmd->plugin.size = info.size();
    memcpy(cmd->plugin.data, info.c_str(), info.size());

    auto chn = bridge_->getNRTChannel();
    if (chn.addCommand(cmd, cmdSize)){
        LOG_DEBUG("PluginClient (" << id_ << "): wait for Server");
        chn.send();
    } else {
        // info too large, try to transmit via tmp file
        LOG_DEBUG("PluginClient (" << id_ << "): send info via tmp file (" << info.size() << " bytes)");
        std::stringstream ss;
        ss << getTmpDirectory() << "/vst_" << (void *)this;
        std::string path = ss.str();
        TmpFile file(path, File::WRITE);
        if (!file){
            throw Error(Error::SystemError,
                        "PluginClient: couldn't create tmp file");
        }
        file << info;
        if (!file){
            throw Error(Error::SystemError,
                        "PluginClient: couldn't write info to tmp file");
        }

        cmdSize = sizeof(ShmCommand) + path.size() + 1;
        cmd->plugin.size = 0; // !
        memcpy(cmd->plugin.data, path.c_str(), path.size() + 1);

        if (!chn.addCommand(cmd, cmdSize)){
            throw Error(Error::PluginError,
                        "PluginClient: couldn't send plugin info");
        }

        LOG_DEBUG("PluginClient (" << id_ << "): wait for Server");
        chn.send(); // tmp file is still in scope!
    }

    // in case the process has already crashed during creation...
    if (!bridge_->alive()){
        throw Error(Error::PluginError, "plugin crashed");
    }

    LOG_DEBUG("PluginClient (" << id_ << "): plugin created");

    // collect replies (after check!)
    const ShmCommand *reply;
    while (chn.getReply(reply)){
        dispatchReply(*reply);
    }

    if (editor && info_->editor()) {
        window_ = std::make_unique<WindowClient>(*this);
    }

    LOG_DEBUG("PluginClient (" << id_ << "): done!");
}

PluginClient::~PluginClient(){
    if (listener_){
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
    LOG_DEBUG("PluginClient (" << id_ << "): free");
}

bool PluginClient::check() {
    return bridge_->alive();
}

void PluginClient::setupProcessing(double sampleRate, int maxBlockSize,
                                   ProcessPrecision precision, ProcessMode mode){
    if (!check()){
        return;
    }
    LOG_DEBUG("PluginClient (" << id_ << "): setupProcessing");

    ShmCommand cmd(Command::SetupProcessing);
    cmd.id = id();
    cmd.setup.sampleRate = sampleRate;
    cmd.setup.maxBlockSize = maxBlockSize;
    cmd.setup.precision = static_cast<uint8_t>(precision);
    cmd.setup.mode = static_cast<uint8_t>(mode);

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
    LOG_PROCESS("PluginClient (" << id_ << "): start processing");

    auto channel = bridge().getRTChannel();

    LOG_PROCESS("PluginClient (" << id_ << "): send process command");
    // send process command
    ShmCommand cmd(Command::Process);
    cmd.id = id();
    cmd.process.numSamples = data.numSamples;
    cmd.process.precision = (uint8_t)data.precision;
    cmd.process.mode = (uint8_t)data.mode;
    cmd.process.numInputs = data.numInputs;
    cmd.process.numOutputs = data.numOutputs;

    channel.AddCommand(cmd, process);

    // send input busses
    for (int i = 0; i < data.numInputs; ++i){
        auto& bus = data.inputs[i];
        // write all channels sequentially to avoid additional copying.
        LOG_PROCESS("PluginClient (" << id_ << "): write input bus " << i << " with "
                    << bus.numChannels << " channels");
        for (int j = 0; j < bus.numChannels; ++j){
            channel.addCommand((const T *)bus.channelData32[j], sizeof(T) * data.numSamples);
        }
    }

    // add commands (parameter changes, MIDI messages, etc.)
    LOG_PROCESS("PluginClient (" << id_ << "): send commands");
    sendCommands(channel);

    // send and wait for reply
    LOG_PROCESS("PluginClient (" << id_ << "): wait");
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
        LOG_PROCESS("PluginClient (" << id_ << "): read output bus " << i << " with "
                    << bus.numChannels << " channels");
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
                LOG_ERROR("PluginClient (" << id_ << "): missing channel " << j
                          << " for audio output bus " << i);
            }
        }
    }

    // get replies (parameter changes, MIDI messages, etc.)
    LOG_PROCESS("PluginClient (" << id_ << "): read replies");
    const ShmCommand* reply;
    while (channel.getReply(reply)){
        dispatchReply(*reply);
    }
    LOG_PROCESS("PluginClient (" << id_ << "): finished processing");
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
    // LOG_DEBUG("PluginClient (" << id_ << "): got reply " << reply.type);
    switch (reply.type){
    case Command::ParamAutomated:
    case Command::ParameterUpdate:
    {
        auto index = reply.paramState.index;
        auto value = reply.paramState.value;

        paramCache_[index].value = value;
        paramCache_[index].display = reply.paramState.display;

        if (reply.type == Command::ParamAutomated){
            if (listener_){
                listener_->parameterAutomated(index, value);
            }
            LOG_DEBUG("PluginClient (" << id_ << "): parameter " << index
                      << " automated ");
        } else {
            LOG_DEBUG("PluginClient (" << id_ << "): parameter " << index
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
        latency_ = reply.i;
        if (listener_){
            listener_->latencyChanged(latency_);
        }
        break;
    case Command::UpdateDisplay:
        if (listener_){
            listener_->updateDisplay();
        }
        break;
    case Command::MidiReceived:
        if (listener_){
            listener_->midiEvent(reply.midi);
        }
        break;
    case Command::SysexReceived:
        if (listener_){
            SysexEvent sysex;
            sysex.delta = reply.sysex.delta;
            sysex.size = reply.sysex.size;
            sysex.data = reply.sysex.data;

            listener_->sysexEvent(sysex);
        }
        break;
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
    LOG_DEBUG("PluginClient (" << id_ << "): suspend");
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

    LOG_DEBUG("PluginClient (" << id_ << "): resume");
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

void PluginClient::setListener(IPluginListener* listener) {
    listener_ = listener;
    if (listener){
        if (bridge_->alive()){
            bridge_->addUIClient(id_, listener);
        } else {
            // in case the plugin has crashed during setup,
            // but we didn't have a chance to get a notification
            listener->pluginCrashed();
        }
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

    Command cmd(Command::SetProgramName);
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
    LOG_DEBUG("PluginClient (" << id_ << "): readProgramFile");
    sendFile(Command::ReadProgramFile, path);
}

void PluginClient::readProgramData(const char *data, size_t size){
    LOG_DEBUG("PluginClient (" << id_ << "): readProgramData");
    sendData(Command::ReadProgramData, data, size);
}

void PluginClient::readBankFile(const std::string& path){
    LOG_DEBUG("PluginClient (" << id_ << "): readBankFile");
    sendFile(Command::ReadBankFile, path);
}

void PluginClient::readBankData(const char *data, size_t size){
    LOG_DEBUG("PluginClient (" << id_ << "): readBankData");
    sendData(Command::ReadBankData, data, size);
}

void PluginClient::writeProgramFile(const std::string& path){
    LOG_DEBUG("PluginClient (" << id_ << "): writeProgramFile");
    sendFile(Command::WriteProgramFile, path);
}

void PluginClient::writeProgramData(std::string& buffer){
    LOG_DEBUG("PluginClient (" << id_ << "): writeProgramData");
    receiveData(Command::WriteProgramData, buffer);
}

void PluginClient::writeBankFile(const std::string& path){
    LOG_DEBUG("PluginClient (" << id_ << "): writeBankFile");
    sendFile(Command::WriteBankFile, path);
}

void PluginClient::writeBankData(std::string& buffer){
    LOG_DEBUG("PluginClient (" << id_ << "): writeBankData");
    receiveData(Command::WriteBankData, buffer);
}

void PluginClient::sendFile(Command::Type type, const std::string &path) {
    if (!check()){
        return;
    }

    auto pathSize = path.size() + 1;
    auto cmdSize = CommandSize(ShmCommand, buffer, pathSize);
    auto cmd = (ShmCommand *)alloca(cmdSize);
    new (cmd) ShmCommand(type, id());
    cmd->buffer.size = pathSize;
    memcpy(cmd->buffer.data, path.data(), pathSize);

    auto chn = bridge().getNRTChannel();
    if (!chn.addCommand(cmd, cmdSize)) {
        throw Error(Error::PluginError,
                    "PluginClient: could not send file path");
    }
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

void PluginClient::sendData(Command::Type type, const char *data, size_t size){
    if (!check()){
        return;
    }

    auto totalSize = sizeof(ShmCommand) + size;
    auto chn = bridge().getNRTChannel();
    if (totalSize > chn.capacity()) {
        // plugin data too large, try to transmit via tmp file
        LOG_DEBUG("PluginClient (" << id_ << "): send plugin data via tmp file (size: "
                  << size << ", capacity: " << chn.capacity() << ")");
        std::stringstream ss;
        ss << getTmpDirectory() << "/vst_" << (void *)this;
        std::string path = ss.str();
        TmpFile file(path, File::WRITE);
        if (!file){
            throw Error(Error::SystemError,
                        "PluginClient: couldn't create tmp file");
        }
        file.write(data, size);
        if (!file){
            throw Error(Error::SystemError,
                        "PluginClient: couldn't write plugin data to tmp file");
        }
        // avoid dead lock in sendFile()!
        {
            auto dummy = std::move(chn);
        }
        auto cmd = (type == Command::ReadProgramData) ?
                    Command::ReadProgramFile : Command::ReadBankFile;
        sendFile(cmd, path);

        return; // !
    }

    ShmCommand cmd(type, id());
    cmd.i = size; // save actual size!
    chn.AddCommand(cmd, buffer);
    // send data as seperate command to avoid needless copy
    if (!chn.addCommand(data, size)) {
        throw Error("plugin data too large!"); // shouldn't happen
    }

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
            auto realSize = reply->i;
            // data is in a seperate message!
            const char *data;
            size_t size;
            if (chn.getReply(data, size)){
                assert(size >= realSize); // 'size' can be larger because of padding!
                buffer.assign(data, realSize);
            } else {
                throw Error(Error::PluginError,
                            "PluginClient::receiveData: missing data message");
            }
        } else if (reply->type == Command::PluginDataFile) {
            // data is transmitted in a tmp file
            auto path = reply->buffer.data;
            File file(path, File::READ);
            if (!file){
                throw Error(Error::SystemError, "PluginClient: couldn't read tmp file");
            }
            file.seekg(0, std::ios_base::end);
            buffer.resize(file.tellg());
            file.seekg(0, std::ios_base::beg);
            file.read(&buffer[0], buffer.size());
            file.close();

            // we have to remove the tmp file!
            if (!removeFile(path)) {
                LOG_ERROR("PluginClient (" << id_ << "): couldn't remove tmp file");
            }
        } else if (reply->type == Command::Error) {
           reply->throwError();
        } else {
            throw Error(Error::PluginError,
                        "PluginClient::receiveData: unexpected reply message " + std::to_string(reply->type));
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


/*///////////////////// WindowClient ////////////////////*/

WindowClient::WindowClient(PluginClient& plugin)
    : plugin_(&plugin) {}

WindowClient::~WindowClient(){}

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

} // vst
