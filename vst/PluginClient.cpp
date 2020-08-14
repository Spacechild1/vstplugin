#include "PluginClient.h"
#include "Utility.h"

#include <algorithm>

namespace vst {

/*/////////////////////// PluginClient /////////////////////////////*/

#define FORBIDDEN_METHOD(name) throw Error(Error::PluginError, "PluginClient: must not call " name "()");

#define UNSUPPORTED_METHOD(name) LOG_WARNING(name "() not supported with bit bridging");

#define ShmCommandAlloca(type, extra) new(alloca(sizeof(ShmCommand) + extra))ShmCommand(type)

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

    // TODO: open plugin
}

PluginClient::~PluginClient(){
    bridge_->removeUIClient(id_);
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
    ShmNRTCommand cmd(Command::SetupProcessing, id());
    cmd.setup.sampleRate = sampleRate;
    cmd.setup.maxBlockSize = maxBlockSize;
    cmd.setup.precision = static_cast<uint32_t>(precision);

    auto chn = bridge().getNRTChannel();
    chn.addCommand(&cmd, CommandSize(cmd, setup));
    chn.send();
}

template<typename T>
void PluginClient::doProcess(ProcessData<T>& data){
    auto channel = bridge().getRTChannel();

    // set plugin
    {
        ShmCommand cmd(Command::SetPlugin);
        cmd.id = id();

        channel.addCommand(&cmd, CommandSize(cmd, id));
    }

    // send commands (parameter changes, MIDI messages, etc.)
    sendCommands(channel);

    // send process command
    {
        ShmCommand cmd(Command::Process);
        cmd.process.numInputs = data.numInputs;
        cmd.process.numOutputs = data.numOutputs;
        cmd.process.numAuxInputs = data.numAuxInputs;
        cmd.process.numAuxOutpus = data.numAuxOutputs;
        cmd.process.numSamples = data.numSamples;

        channel.addCommand(&cmd, CommandSize(cmd, process));
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

    // get events (parameter changes, MIDI messages, etc.)
    receiveEvents(channel);
}

void PluginClient::sendCommands(RTChannel& channel){

}

void PluginClient::receiveEvents(RTChannel& channel){

}

void PluginClient::process(ProcessData<float>& data){
    doProcess(data);
}

void PluginClient::process(ProcessData<double>& data){
    doProcess(data);
}

void PluginClient::suspend(){
    ShmNRTCommand cmd(Command::Suspend, id());

    auto chn = bridge().getNRTChannel();
    chn.addCommand(&cmd, CommandSize(cmd, empty));
    chn.send();
}

void PluginClient::resume(){
    ShmNRTCommand cmd(Command::Resume, id());

    auto chn = bridge().getNRTChannel();
    chn.addCommand(&cmd, CommandSize(cmd, empty));
    chn.send();
}

void PluginClient::setNumSpeakers(int in, int out, int auxin, int auxout){
    ShmNRTCommand cmd(Command::SetNumSpeakers, id());
    cmd.speakers.in = in;
    cmd.speakers.auxin = auxin;
    cmd.speakers.out = out;
    cmd.speakers.auxout = auxout;

    auto chn = bridge().getNRTChannel();
    chn.addCommand(&cmd, CommandSize(cmd, speakers));
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
    auto cmd = (ShmNRTCommand *)alloca(sizeof(ShmNRTCommand) + size);
    cmd->type = type;
    cmd->id = id();
    memcpy(cmd->buffer.data, data, size);

    auto chn = bridge().getNRTChannel();
    chn.addCommand(cmd, CommandSize(*cmd, buffer) + size);
    chn.send();

    const ShmCommand *reply;
    while (chn.getReply(reply)){
        // TODO parameter changes, program name changes, program change
    }
}

void PluginClient::receiveData(Command::Type type, std::string &buffer){
    ShmNRTCommand cmd(type, id());

    auto chn = bridge().getNRTChannel();
    chn.addCommand(&cmd, CommandSize(cmd, empty));
    chn.send();

    const ShmCommand *reply;
    if (chn.getReply(reply)){
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
    ShmNRTCommand cmd(Command::WindowOpen, plugin_->id());
    plugin_->bridge().postUIThread(cmd);
}

void WindowClient::close(){
    ShmNRTCommand cmd(Command::WindowClose, plugin_->id());
    plugin_->bridge().postUIThread(cmd);
}

void WindowClient::setPos(int x, int y){
    ShmNRTCommand cmd(Command::WindowSetPos, plugin_->id());
    cmd.windowPos.x = x;
    cmd.windowPos.y = y;
    plugin_->bridge().postUIThread(cmd);
}

void WindowClient::setSize(int w, int h){
    ShmNRTCommand cmd(Command::WindowSetSize, plugin_->id());
    cmd.windowSize.width = w;
    cmd.windowSize.height = h;
    plugin_->bridge().postUIThread(cmd);
}

void WindowClient::update(){
    // ignore
}

} // vst
