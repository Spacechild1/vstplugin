#include "SC_PlugIn.hpp"

static InterfaceTable *ft;

class VstPlugin : public SCUnit {
public:
	VstPlugin();
	~VstPlugin(){}
	void next(int inNumSamples);
};

VstPlugin::VstPlugin(){
	set_calc_function<VstPlugin, &VstPlugin::next>();
}

void VstPlugin::next(int inNumSamples){
	const float *left = in(0);
	const float *right = in(1);
	float *result = out(0);

	for (int i = 0; i < inNumSamples; i++){
		result[i] = left[i] + right[i];
	}
}

DEFINE_XTORS(VstPlugin)

// the entry point is called by the host when the plug-in is loaded
PluginLoad(VstPluginUGens) {
    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable
    DefineDtorUnit(VstPlugin);
}
