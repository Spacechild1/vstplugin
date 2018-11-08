#pragma once

#include <string>

class IVSTWindow {
 public:
    virtual ~IVSTWindow() {}

    virtual void* getHandle() = 0; // get system-specific handle to the window

    virtual void setGeometry(int left, int top, int right, int bottom) = 0;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void minimize() = 0;
    virtual void restore() = 0; // un-minimize
    virtual void bringToTop() = 0;
};

class IVSTPlugin {
 public:
    virtual ~IVSTPlugin(){}
    virtual std::string getPluginName() const = 0;
    virtual int getPluginVersion() const = 0;

    virtual void process(float **inputs, float **outputs, int nsamples) = 0;
    virtual void processDouble(double **inputs, double **outputs, int nsamples) = 0;
    virtual bool hasSinglePrecision() const = 0;
    virtual bool hasDoublePrecision() const = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void setSampleRate(float sr) = 0;
    virtual void setBlockSize(int n) = 0;
    virtual int getNumInputs() const = 0;
    virtual int getNumOutputs() const = 0;

    virtual void setParameter(int index, float value) = 0;
    virtual float getParameter(int index) const = 0;
    virtual std::string getParameterName(int index) const = 0;
    virtual std::string getParameterLabel(int index) const = 0;
    virtual std::string getParameterDisplay(int index) const = 0;
    virtual int getNumParameters() const = 0;

    virtual void setProgram(int index) = 0;
    virtual void setProgramName(const std::string& name) = 0;
    virtual int getProgram() = 0;
    virtual std::string getProgramName() const = 0;
    virtual std::string getProgramNameIndexed(int index) const = 0;
    virtual int getNumPrograms() const = 0;

    virtual bool hasEditor() const = 0;
    virtual void openEditor(void *window) = 0;
    virtual void closeEditor() = 0;
    virtual void getEditorRect(int &left, int &top, int &right, int &bottom) const = 0;

    virtual void createWindow() = 0;
    virtual void destroyWindow() = 0;
    virtual IVSTWindow *getWindow() = 0;
};

// expects a path to the actual plugin file (e.g. "myplugin.dll" on Windows,
// "myplugin.so" on Linux, "myplugin.vst/Contents/MacOS/myplugin" on Apple).
// use 'makeVSTPluginFilePath' for a cross platform solution
IVSTPlugin* loadVSTPlugin(const std::string& path);

void freeVSTPlugin(IVSTPlugin* plugin);

// check the path and append platform specific extensions (if needed)
std::string makeVSTPluginFilePath(const std::string& path);
