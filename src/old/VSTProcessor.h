#pragma once

// VST host
 
#include <string>
#include <memory>

class IVSTPlugin;
using VSTPluginPtr = std::unique_ptr<IVSTPlugin>;

class VSTProcessor {
public:
    VSTProcessor();
    ~VSTProcessor();
	
	void openPlugin(const std::string& path);
	void closePlugin();
	bool hasPlugin() const;
    int getPluginVersion() const;

    void process(float **inputs, float **outputs, int nsamples);
    void processDouble(double **inputs, double **outputs, int nsamples);
	
	void setParameter(int index, float value);
	float getParameter(int index) const;
	int getNumParameters() const;
	std::string getParameterName(int index) const;
	
	void setProgram(int program);
	int getProgram();
	int getNumPrograms() const;
	std::string getProgramName() const;
	void setProgramName(const std::string& name);
	
    void showEditor();
    void hideEditor();
    void resizeEditor(int x, int y, int w, int h) const;
private:
    VSTPluginPtr plugin_ = nullptr;
};
