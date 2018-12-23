#include "SC_PlugIn.hpp"

static InterfaceTable *ft;

struct VstPlugin : public SCUnit {
public:
    VstPlugin() {
        // New way of setting calc function.
        set_calc_function<VstPlugin, &VstPlugin::next>();
        next(1);
    }
	~VstPlugin(){
		
    }
private:

    // Calc function
    void next(int inNumSamples) {
        const float* left = in(0);
        const float* right = in(1);
        float* out_l = out(0);
        float* out_r = out(1);

        for (int i = 0; i < inNumSamples; i++) {
            out_l[i] = left[i];
            out_r[i] = right[i];
        }
    }
};

PluginLoad(VstPlugin) {
    ft = inTable;
    registerUnit<VstPlugin>(ft, "VstPlugin");
}
