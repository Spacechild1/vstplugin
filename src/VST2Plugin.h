// VST2Plugin

#include "VSTPlugin.h"

#include "aeffect.h"
#include "aeffectx.h"
#include "vstfxstore.h"

// Plugin's entry point
typedef AEffect *(*vstPluginFuncPtr)(audioMasterCallback);

// AEffectDispatcherProc
// AEffectProcessProc
// AEffectSetParameterProc
// AEffectGetParameterProc

class VST2Plugin final : public VSTPlugin {
public:
    static VstIntPtr VSTCALLBACK hostCallback(AEffect *plugin, VstInt32 opcode,
        VstInt32 index, VstInt32 value, void *ptr, float opt);

    VST2Plugin(void* plugin, const std::string& path);
    ~VST2Plugin();
    std::string getPluginName() const override;
    int getPluginVersion() const override;

    void process(float **inputs, float **outputs, int nsamples) override;
    void processDouble(double **inputs, double **outputs, int nsamples) override;
    bool hasSinglePrecision() const override;
    bool hasDoublePrecision() const override;
    void pause() override;
    void resume() override;
    void setSampleRate(float sr) override;
    void setBlockSize(int n) override;
    int getNumInputs() const override;
    int getNumOutputs() const override;

    void setParameter(int index, float value) override;
    float getParameter(int index) const override;
    std::string getParameterName(int index) const override;
    std::string getParameterLabel(int index) const override;
    std::string getParameterDisplay(int index) const override;
    int getNumParameters() const override;

    void setProgram(int program) override;
    void setProgramName(const std::string& name) override;
    int getProgram() override;
    std::string getProgramName() const override;
    std::string getProgramNameIndexed(int index) const override;
    int getNumPrograms() const override;

    bool hasEditor() const override;
    void openEditor(void *window) override;
    void closeEditor() override;
    void getEditorRect(int &left, int &top, int &right, int &bottom) const override;
private:
    bool hasFlag(VstAEffectFlags flag) const;
    VstIntPtr dispatch(VstInt32 opCode, VstInt32 index = 0, VstInt32 value = 0,
                       void *ptr = 0, float opt = 0) const;
    // data members
    AEffect *plugin_ = nullptr;
};


