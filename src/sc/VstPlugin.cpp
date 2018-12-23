#include "SC_PlugIn.h"

static InterfaceTable *ft;

struct VstPlugin : public Unit {
	VstPlugin();
	~VstPlugin(){}
};

static void VstPlugin_next(VstPlugin* unit, int inNumSamples);

VstPlugin::VstPlugin(){
	VstPlugin *unit = this;
	SETCALC(VstPlugin_next);
	VstPlugin_next(unit, 1);
}

void VstPlugin_next(VstPlugin* unit, int inNumSamples) {
    float *left = IN(0);
    float *right = IN(1);
    float *out = OUT(0);
    
    for (int i = 0; i < inNumSamples; i++) {
        out[i] = left[i] + right[i];
    }
}

void VstPlugin_Ctor(VstPlugin* unit) {
	unit = new(unit) VstPlugin();
}

void VstPlugin_Dtor(VstPlugin* unit) {
	unit->~VstPlugin();
}

// the entry point is called by the host when the plug-in is loaded
PluginLoad(VstPluginUGens) {
    // InterfaceTable *inTable implicitly given as argument to the load function
    ft = inTable; // store pointer to InterfaceTable
    DefineDtorUnit(VstPlugin);
}
