#include "VSTProcessor.h"


VSTProcessor::VSTProcessor();
VSTProcessor::~VSTProcessor();

void VSTProcessor::openPlugin(const std::string& path);
void VSTProcessor::closePlugin();
bool VSTProcessor::hasPlugin() const;
int VSTProcessor::getPluginVersion() const;

void VSTProcessor::process(float **inputs, float **outputs, int nsamples);
void VSTProcessor::processDouble(double **inputs, double **outputs, int nsamples);

void VSTProcessor::setParameter(int index, float value);
float VSTProcessor::getParameter(int index) const;
int VSTProcessor::getNumParameters() const;
std::string VSTProcessor::getParameterName(int index) const;

void VSTProcessor::setProgram(int program);
int VSTProcessor::getProgram();
int VSTProcessor::getNumPrograms() const;
std::string VSTProcessor::getProgramName() const;
void VSTProcessor::setProgramName(const std::string& name);

void VSTProcessor::showEditor();
void VSTProcessor::hideEditor();
void VSTProcessor::resizeEditor(int x, int y, int w, int h) const;
