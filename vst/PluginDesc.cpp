#include "PluginDesc.h"

#include "CpuArch.h"
#include "MiscUtils.h"
#include "FileUtils.h"
#include "Log.h"
#include "Sync.h"

#include <algorithm>
#include <sstream>
#include <cstring>

namespace vst {

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

const char * getVersionString(){
    static std::string version = [](){
        std::stringstream ss;
        ss << VERSION_MAJOR << "." << VERSION_MINOR;
        if (VERSION_PATCH > 0){
            ss << "." << VERSION_PATCH;
        }
        if (VERSION_PRERELEASE > 0){
            ss << "-pre" << VERSION_PRERELEASE;
        }
        return ss.str();
    }();
    return version.c_str();
}

/*////////////////////// preset ///////////////////////*/

static SharedMutex gFileLock;
#if !defined(_WIN32) && !defined(__APPLE__)
static bool gDidCreateVstFolders = false;
#endif

static std::string getPresetLocation(PresetType presetType, PluginType pluginType){
    std::string result;
#if defined(_WIN32)
    switch (presetType){
    case PresetType::User:
        result = expandPath("%USERPROFILE%") + "\\Documents";
        break;
    case PresetType::UserFactory:
        result = expandPath("%APPDATA%");
        break;
    case PresetType::SharedFactory:
        result = expandPath("%PROGRAMDATA%");
        break;
    default:
        return "";
    }
    if (pluginType == PluginType::VST3){
        return result + "\\VST3 Presets";
    } else {
        return result + "\\VST2 Presets";
    }
#elif defined(__APPLE__)
    switch (presetType){
    case PresetType::User:
        return expandPath("~/Library/Audio/Presets");
    case PresetType::SharedFactory:
        return "/Library/Audio/Presets";
    default:
        return "";
    }
#else
    switch (presetType){
    case PresetType::User:
    {
        result = expandPath("~/.");
        // VST directories might not exist yet
        ReadLock rdlock(gFileLock);
        if (!gDidCreateVstFolders){
            rdlock.unlock();
            WriteLock wrlock(gFileLock);
            // LATER do some error handling
        #if USE_VST2
            createDirectory(result + "vst");
        #endif
        #if USE_VST3
            createDirectory(result + "vst3");
        #endif
            gDidCreateVstFolders = true;
        }
        break;
    }
    case PresetType::SharedFactory:
        result = "/usr/local/share/";
        break;
    case PresetType::Global:
        result = "/usr/share/";
        break;
    default:
        return "";
    }
    if (pluginType == PluginType::VST3){
        return result + "vst3/presets";
    } else {
        return result + "vst/presets";
    }
#endif
}

/*///////////////////// PluginDesc /////////////////////*/

PluginDesc::PluginDesc(std::shared_ptr<const IFactory> f){
    if (f){
        setFactory(std::move(f));
    }
}

PluginDesc::~PluginDesc(){}

void PluginDesc::setFactory(std::shared_ptr<const IFactory> factory){
    if (path_.empty()){
        path_ = factory->path();
    }
    if (factory->arch() != getHostCpuArchitecture()){
        flags |= Bridged;
    }
    factory_ = std::move(factory);
}

CpuArch PluginDesc::arch() const {
    auto factory = factory_.lock();
    return factory ? factory->arch() : CpuArch::unknown;
}

// ThreadedPlugin.cpp
IPlugin::ptr makeThreadedPlugin(IPlugin::ptr plugin);

#if USE_BRIDGE
// PluginClient.cpp
IPlugin::ptr makeBridgedPlugin(IFactory::const_ptr factory, const std::string& name,
                               bool editor, bool sandbox);
#endif

IPlugin::ptr PluginDesc::create(bool editor, bool threaded, RunMode mode) const {
    std::shared_ptr<const IFactory> factory = factory_.lock();
    if (!factory){
        return nullptr;
    }
    IPlugin::ptr plugin;
#if USE_BRIDGE
    if ((mode == RunMode::Bridge) || (mode == RunMode::Sandbox) ||
            ((mode == RunMode::Auto) && bridged())){
        plugin = makeBridgedPlugin(factory, name, editor, mode == RunMode::Sandbox);
    }
    else
#endif
    {
        plugin = factory->create(name);
        if (editor && plugin->info().editor()){
            auto window = IWindow::create(*plugin);
            plugin->setWindow(std::move(window));
        }
    }

    if (threaded){
        plugin = makeThreadedPlugin(std::move(plugin));
    }

    return plugin;
}

#if USE_VST2
void PluginDesc::setUniqueID(int _id){
    type_ = PluginType::VST2;
    char buf[9];
    // should we write in little endian?
    snprintf(buf, sizeof(buf), "%08X", _id);
    buf[8] = 0;
    uniqueID = buf;
    id_.id = _id;
}
#endif

#if USE_VST3
void PluginDesc::setUID(const char *uid){
    type_ = PluginType::VST3;
    char buf[33];
    for (int i = 0; i < 16; ++i){
        // we have to cast to uint8_t!
        sprintf(buf + 2 * i, "%02X", (uint8_t)uid[i]);
    }
    buf[32] = 0;
    uniqueID = buf;
    memcpy(id_.uid, uid, 16);
}
#endif

WriteLock PluginDesc::writeLock(){
    return WriteLock(*mutex);
}

ReadLock PluginDesc::readLock() const {
    return ReadLock(*mutex);
}

static void conformPath(std::string& path){
    // replace backslashes
    for (auto& c : path){
        if (c == '\\'){
            c = '/';
        }
    }
}

void PluginDesc::addParameter(Param param){
    auto index = parameters.size();
    // inverse mapping
    paramMap_[param.name] = index;
#if USE_VST3
    // index -> ID mapping
    indexToIdMap_[index] = param.id;
    // ID -> index mapping
    idToIndexMap_[param.id] = index;
#endif
    // add parameter
    parameters.push_back(std::move(param));
}

int PluginDesc::findParam(const std::string& key) const {
    auto it = paramMap_.find(key);
    if (it != paramMap_.end()){
        return it->second;
    } else {
        return -1;
    }
}

#if USE_VST3
uint32_t PluginDesc::getParamID(int index) const {
    auto it = indexToIdMap_.find(index);
    if (it != indexToIdMap_.end()){
        return it->second;
    }
    else {
        return 0; // throw?
    }
}

int PluginDesc::getParamIndex(uint32_t _id) const {
    auto it = idToIndexMap_.find(_id);
    if (it != idToIndexMap_.end()){
        return it->second;
    }
    else {
        return -1; // not automatable
    }
}
#endif

void PluginDesc::scanPresets(){
    const std::vector<PresetType> presetTypes = {
#if defined(_WIN32)
        PresetType::User, PresetType::UserFactory, PresetType::SharedFactory
#elif defined(__APPLE__)
        PresetType::User, PresetType::SharedFactory
#else
        PresetType::User, PresetType::SharedFactory, PresetType::Global
#endif
    };
    PresetList results;
    for (auto& presetType : presetTypes){
        auto folder = getPresetFolder(presetType);
        if (pathExists(folder)){
            vst::search(folder, [&](const std::string& path){
                auto ext = fileExtension(path);
                if ((type_ == PluginType::VST3 && ext != ".vstpreset") ||
                    (type_ == PluginType::VST2 && ext != ".fxp" && ext != ".FXP")){
                    return;
                }
                Preset preset;
                preset.type = presetType;
                preset.name = fileBaseName(path);
                preset.path = path;
            #ifdef _WIN32
                conformPath(preset.path);
            #endif
                results.push_back(std::move(preset));
            }, false);
        }
    }
    presets = std::move(results);
    sortPresets(false);
#if 0
    if (numPresets()){
        LOG_VERBOSE("presets:");
        for (auto& preset : presets){
            LOG_VERBOSE("\t" << preset.path);
        }
    }
#endif
}

void PluginDesc::sortPresets(bool userOnly){
    // don't lock! private method
    auto it1 = presets.begin();
    auto it2 = it1;
    if (userOnly){
        // get iterator past user presets
        while (it2 != presets.end() && it2->type == PresetType::User) ++it2;
    } else {
        it2 = presets.end();
    }
    std::sort(it1, it2, [](const auto& lhs, const auto& rhs) {
        return stringCompare(lhs.name, rhs.name);
    });
}

int PluginDesc::findPreset(const std::string &name) const {
    for (int i = 0; i < presets.size(); ++i){
        if (presets[i].name == name){
            return i;
        }
    }
    return -1;
}

bool PluginDesc::removePreset(int index, bool del){
    if (index >= 0 && index < presets.size()
            && presets[index].type == PresetType::User
            && (!del || removeFile(presets[index].path))){
        presets.erase(presets.begin() + index);
        return true;
    }
    return false;
}

bool PluginDesc::renamePreset(int index, const std::string& newName){
    // make preset before creating the lock!
    if (index >= 0 && index < presets.size()
            && presets[index].type == PresetType::User){
        auto preset = makePreset(newName);
        if (!preset.name.empty()){
            if (renameFile(presets[index].path, preset.path)){
                presets[index] = std::move(preset);
                sortPresets();
                return true;
            }
        }
    }
    return false;
}

static std::string bashPath(std::string path){
    for (auto& c : path){
        switch (c){
        case '/':
        case '\\':
        case '\"':
        case '?':
        case '*':
        case ':':
        case '<':
        case '>':
        case '|':
            c = '_';
            break;
        default:
            break;
        }
    }
    return path;
}

int PluginDesc::addPreset(Preset preset) {
    auto it = presets.begin();
    // insert lexicographically
    while (it != presets.end() && it->type == PresetType::User){
        if (preset.name == it->name){
            // replace
            *it = std::move(preset);
            return (int)(it - presets.begin());
        }
        if (stringCompare(preset.name, it->name)){
            break;
        }
        ++it;
    }
    int index = (int)(it - presets.begin()); // before reallocation!
    presets.insert(it, std::move(preset));
    return index;
}

Preset PluginDesc::makePreset(const std::string &name, PresetType type) const {
    Preset preset;
    auto folder = getPresetFolder(type, true);
    if (!folder.empty()){
        preset.path = folder + "/" + bashPath(name) +
                (type_ == PluginType::VST3 ? ".vstpreset" : ".fxp");
        preset.name = name;
        preset.type = type;
    }
    return preset;
}

std::string PluginDesc::getPresetFolder(PresetType type, bool create) const {
    auto location = getPresetLocation(type, type_);
    if (!location.empty()){
        auto vendorFolder = location + "/" + bashPath(vendor);
        auto pluginFolder = vendorFolder + "/" + bashPath(name);
        // create folder(s) if necessary
        if (create && !didCreatePresetFolder && type == PresetType::User){
            // LATER do some error handling
            createDirectory(location);
            createDirectory(vendorFolder);
            createDirectory(pluginFolder);
            didCreatePresetFolder = true;
        }
    #ifdef _WIN32
        conformPath(pluginFolder);
    #endif
        return pluginFolder;
    } else {
        return "";
    }
}

/// .ini file structure for each plugin:
///
/// [plugin]
/// path=<string>
/// name=<string>
/// vendor=<string>
/// category=<string>
/// version=<string>
/// sdkversion=<string>
/// id=<int>
/// inputs=<int>
/// outputs=<int>
/// flags=<int>
/// [parameters]
/// n=<int>
/// name,label
/// name,label
/// ...
/// [programs]
/// n=<int>
/// <program0>
/// <program1>
/// <program2>
/// ...

static std::string bashString(std::string name){
    // replace "forbidden" characters
    for (auto& c : name){
        switch (c){
        case ',':
        case '\n':
        case '\r':
            c = '_';
            break;
        default:
            break;
        }
    }
    return name;
}

#define toHex(x) std::hex << (x) << std::dec
#define fromHex(x) std::stol(x, 0, 16); // hex

void writeBusses(std::ostream& file, const std::vector<PluginDesc::Bus>& vec){
    int n = vec.size();
    file << "n=" << n << "\n";
    for (int i = 0; i < n; ++i){
        file << vec[i].numChannels << ","
             << (int)vec[i].type << ","
             << bashString(vec[i].label) << "\n";
    }
}

void PluginDesc::serialize(std::ostream& file) const {
    // list sub plugins (only when probing)
    if (!subPlugins.empty()){
        file << "[subplugins]\n";
        file << "n=" << (int)subPlugins.size() << "\n";
        for (auto& sub : subPlugins){
            file << sub.name << "," << toHex(sub.id) << "\n";
        }
        return;
    }
    file << "[plugin]\n";
    file << "path=" << path() << "\n";
    file << "id=" << uniqueID << "\n";
    file << "name=" << name << "\n";
    file << "vendor=" << vendor << "\n";
    file << "category=" << category << "\n";
    file << "version=" << version << "\n";
    file << "sdkversion=" << sdkVersion << "\n";
    file << "flags=" << toHex(flags) << "\n";
#if USE_VST3
    if (programChange != NoParamID){
        file << "pgmchange=" << toHex(programChange) << "\n";
    }
    if (bypass != NoParamID){
        file << "bypass=" << toHex(bypass) << "\n";
    }
#endif
    // inputs
    file << "[inputs]\n";
    writeBusses(file, inputs);
    // outputs
    file << "[outputs]\n";
    writeBusses(file, outputs);
    // parameters
    file << "[parameters]\n";
    file << "n=" << parameters.size() << "\n";
    for (auto& param : parameters) {
        file << bashString(param.name) << ","
             << bashString(param.label) << ","
             << toHex(param.id) << "\n";
    }
    // programs
    file << "[programs]\n";
    file << "n=" << programs.size() << "\n";
    for (auto& pgm : programs) {
        file << pgm << "\n";
    }
}

namespace {

std::string ltrim(std::string str){
    auto pos = str.find_first_not_of(" \t");
    if (pos != std::string::npos && pos > 0){
        return str.substr(pos);
    } else {
        return str;
    }
}

std::string rtrim(std::string str){
    auto pos = str.find_last_not_of(" \t") + 1;
    if (pos != std::string::npos && pos < str.size()){
        return str.substr(0, pos);
    } else {
        return str;
    }
}

bool isComment(const std::string& line){
    auto c = line.front();
    return c == ';' || c == '#';
}

void getKeyValuePair(const std::string& line, std::string& key, std::string& value){
    auto pos = line.find('=');
    if (pos == std::string::npos){
        throw Error("missing '=' after key: " + line);
    }
    key = rtrim(line.substr(0, pos));
    value = ltrim(line.substr(pos + 1));
}

std::vector<std::string> splitString(const std::string& str, char sep){
    std::vector<std::string> result;
    auto pos = 0;
    while (true){
        auto newpos = str.find(sep, pos);
        if (newpos != std::string::npos){
            int len = newpos - pos;
            result.push_back(str.substr(pos, len));
            pos = newpos + 1;
        } else {
            result.push_back(str.substr(pos)); // remaining string
            break;
        }
    }
    return result;
}

void parseArg(int32_t& lh, const std::string& rh){
    lh = std::stol(rh); // decimal
}

void parseArg(uint32_t& lh, const std::string& rh){
    lh = fromHex(rh);
}

void parseArg(std::string& lh, const std::string& rh){
    lh = rh;
}

} // namespace

bool getLine(std::istream& stream, std::string& line){
    std::string temp;
    while (std::getline(stream, temp)){
        if (!temp.empty() && !isComment(temp)){
            line = std::move(temp);
            return true;
        }
    }
    return false;
}

int getCount(const std::string& line){
    auto pos = line.find('=');
    if (pos == std::string::npos){
        throw Error("missing '=' after key: " + line);
    }
    try {
        return std::stol(line.substr(pos + 1));
    }
    catch (...){
        throw Error("expected number after 'n='");
    }
}

std::vector<PluginDesc::Bus> readBusses(std::istream& file){
    std::vector<PluginDesc::Bus> result;
    std::string line;
    std::getline(file, line);
    int n = getCount(line);
    while (n-- && std::getline(file, line)){
        auto args = splitString(line, ',');
        PluginDesc::Bus bus;
        bus.numChannels = std::stol(args[0]);
        bus.type = (PluginDesc::Bus::Type)std::stol(args[1]);
        bus.label = ltrim(args[2]);
        result.push_back(std::move(bus));
    }
    return result;
}

void PluginDesc::deserialize(std::istream& file, int versionMajor,
                             int versionMinor, int versionBugfix) {
    // first check for sections, then for keys!
    bool start = false;
    bool future = (versionMajor > VERSION_MAJOR) ||
                  (versionMajor == VERSION_MAJOR && versionMinor > VERSION_MINOR);
    std::string line;
    while (getLine(file, line)){
        if (line == "[plugin]"){
            start = true;
        } else if (line == "[inputs]"){
            inputs = readBusses(file);
        } else if (line == "[outputs]"){
            outputs = readBusses(file);
        } else if (line == "[parameters]"){
            parameters.clear();
            std::getline(file, line);
            int n = getCount(line);
            while (n-- && std::getline(file, line)){
                auto args = splitString(line, ',');
                Param param;
                if (args.size() >= 2){
                    param.name = rtrim(args[0]);
                    param.label = ltrim(args[1]);
                }
                if (args.size() >= 3){
                    param.id = fromHex(args[2]);
                }
                parameters.push_back(std::move(param));
            }
            // inverse mapping name -> index
            for (int i = 0; i < (int)parameters.size(); ++i){
                auto& param = parameters[i];
                paramMap_[param.name] = i;
            #if USE_VST3
                // for VST3:
                idToIndexMap_[param.id] = i;
                indexToIdMap_[i] = param.id;
            #endif
            }
        } else if (line == "[programs]"){
            programs.clear();
            std::getline(file, line);
            int n = getCount(line);
            while (n-- && std::getline(file, line)){
                programs.push_back(std::move(line));
            }
            goto done;
        } else if (line == "[subplugins]"){
            // get list of subplugins (only when probing)
            subPlugins.clear();
            std::getline(file, line);
            int n = getCount(line);
            while (n-- && std::getline(file, line)){
                auto pos = line.find(',');
                SubPlugin sub;
                sub.name = rtrim(line.substr(0, pos));
                sub.id = fromHex(line.substr(pos + 1));
                // LOG_DEBUG("got subplugin " << sub.name << " " << sub.id);
                subPlugins.push_back(std::move(sub));
            }
            goto done;
        } else if (start){
            std::string key;
            std::string value;
            getKeyValuePair(line, key, value);

        #define MATCH(name, field) else if (name == key) { parseArg(field, value); }
        #define IGNORE(name) else if (name == key) {}
            try {
                if (key == "id"){
                    if (value.size() == 8){
                        type_ = PluginType::VST2;
                        sscanf(&value[0], "%08X", &id_.id);
                    } else if (value.size() == 32){
                        type_ = PluginType::VST3;
                        for (int i = 0; i < 16; ++i){
                            unsigned int temp;
                            sscanf(&value[i * 2], "%02X", &temp);
                            id_.uid[i] = temp;
                        }
                    } else {
                        throw Error("bad id!");
                    }
                    uniqueID = value;
                }
                MATCH("path", path_)
                MATCH("name", name)
                MATCH("vendor", vendor)
                MATCH("category", category)
                MATCH("version", version)
                MATCH("sdkversion", sdkVersion)
            #if USE_VST3
                MATCH("pgmchange", programChange)
                MATCH("bypass", bypass)
            #else
                IGNORE("pgmchange")
                IGNORE("bypass")
            #endif
                MATCH("flags", flags) // hex
                else {
                    if (future){
                        LOG_WARNING("unknown key: " << key);
                    } else {
                        throw Error("unknown key: " + key);
                    }
                }
            } catch (const Error&){
                throw; // rethrow
            } catch (const std::invalid_argument& e) {
                throw Error("invalid argument for key '" + key + "': " + value);
            } catch (const std::out_of_range& e) {
                throw Error("out of range argument for key '" + key + "': " + value);
            } catch (const std::exception& e){
                throw Error("unknown error: " + std::string(e.what()));
            }
        } else {
            if (future){
                LOG_WARNING("bad data: " << line);
            } else {
                throw Error("bad data: " + line);
            }
        }
    }
done:
    // restore "Bridge" flag
    auto factory = factory_.lock();
    if (factory && factory->arch() != getHostCpuArchitecture()){
        flags |= Bridged;
    }
}

} // vst
