#include "VSTPluginInterface.h"

#include "m_pd.h"

#include <memory>
#include <cstdio>

#undef pd_class
#define pd_class(x) (*(t_pd *)(x))
#define classname(x) (class_getname(pd_class(x)))

// vsthost~ object
static t_class *vsthost_class;

struct t_vstparam;

struct t_vsthost {
	t_object x_obj;
	t_sample x_f;
    t_outlet *x_messout;
    // VST plugin
    IVSTPlugin* x_plugin;
    int x_bypass;
    int x_blocksize;
    int x_sr;
    int x_dp; // use double precision
    // Pd editor
    t_canvas *x_editor;
    int x_nparams;
    t_vstparam *x_param_vec;
    int x_generic;
    // input signals from Pd
    int x_nin;
    t_float **x_invec;
    // contiguous input buffer
    int x_inbufsize;
    char *x_inbuf;
    // array of pointers into the input buffer
    int x_ninbuf;
    void **x_inbufvec;
    // output signals from Pd
    int x_nout;
    t_float **x_outvec;
    // contiguous output buffer
    int x_outbufsize;
    char *x_outbuf;
    // array of pointers into the output buffer
    int x_noutbuf;
    void **x_outbufvec;
};

// VST parameter responder (for Pd GUI)
static t_class *vstparam_class;

struct t_vstparam {
    t_pd p_pd;
    t_vsthost *p_owner;
    t_symbol *p_name;
    t_symbol *p_display;
    int p_index;
};

static void vsthost_param_set(t_vsthost *x, t_floatarg index, t_floatarg value);

static void vstparam_float(t_vstparam *x, t_floatarg f){
    vsthost_param_set(x->p_owner, x->p_index, f);
}

static void vstparam_set(t_vstparam *x, t_floatarg f){
    // this while set the slider and implicitly call vstparam_doset
    pd_vmess(x->p_name->s_thing, gensym("set"), (char *)"f", f);
}

static void vstparam_doset(t_vstparam *x, t_floatarg f){
    // this method updates the display next to the label
    IVSTPlugin *plugin = x->p_owner->x_plugin;
    int index = x->p_index;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %s", plugin->getParameterDisplay(index).c_str(),
             plugin->getParameterLabel(index).c_str());
    pd_vmess(x->p_display->s_thing, gensym("label"), (char *)"s", gensym(buf));
}

static void vstparam_init(t_vstparam *x, t_vsthost *y, int index){
    x->p_pd = vstparam_class;
    x->p_owner = y;
    x->p_index = index;
    char buf[64];
    snprintf(buf, sizeof(buf), "%lx-hsl-%d", y, index);
    x->p_name = gensym(buf);
    pd_bind((t_pd *)x, x->p_name);
    // post("parameter nr. %d: %s", index, x->p_name->s_name);
    snprintf(buf, sizeof(buf), "%lx-cnv-%d", y, index);
    x->p_display = gensym(buf);
}

static void vstparam_free(t_vstparam *x){
    pd_unbind((t_pd *)x, x->p_name);
}

static void vstparam_setup(void){
    vstparam_class = class_new(gensym("__vstparam"), 0, 0, sizeof(t_vstparam), 0, A_NULL);
    class_addfloat(vstparam_class, (t_method)vstparam_float);
    class_addmethod(vstparam_class, (t_method)vstparam_doset, gensym("set"), A_DEFFLOAT, 0);
}

/**** public interface ****/

// helper functions
static bool vsthost_check(t_vsthost *x){
    if (x->x_plugin){
		return true;
	} else {
        pd_error(x, "%s: no plugin loaded!", classname(x));
		return false;
	}
}

// close
static void vsthost_close(t_vsthost *x){
    if (x->x_plugin){
        freeVSTPlugin(x->x_plugin);
        x->x_plugin = nullptr;
    }
}

// open
static void vsthost_update_buffer(t_vsthost *x);
static void vsthost_make_editor(t_vsthost *x);

static void vsthost_open(t_vsthost *x, t_symbol *s){
    vsthost_close(x);
    IVSTPlugin *plugin = loadVSTPlugin(s->s_name);
    if (plugin){
        post("loaded VST plugin '%s'", plugin->getPluginName().c_str());
        // plugin->setBlockSize(x->x_blocksize);
        // plugin->setSampleRate(x->x_sr);
        // plugin->resume();
        if (plugin->hasSinglePrecision()){
            post("plugin supports single precision");
        }
        if (plugin->hasDoublePrecision()){
            post("plugin supports double precision");
        }
        x->x_plugin = plugin;
        vsthost_update_buffer(x);
        vsthost_make_editor(x);
    } else {
        post("no plugin");
        pd_error(x, "%s: couldn't open plugin %s", classname(x), s->s_name);
    }
}

// bypass
static void vsthost_bypass(t_vsthost *x, t_floatarg f){
    x->x_bypass = (f != 0);
}

// editor
static void editor_mess(t_vsthost *x, t_symbol *sel, int argc = 0, t_atom *argv = 0){
    pd_typedmess((t_pd *)x->x_editor, sel, argc, argv);
}

template<typename... T>
static void editor_vmess(t_vsthost *x, t_symbol *sel, const char *fmt, T... args){
    pd_vmess((t_pd *)x->x_editor, sel, (char *)fmt, args...);
}

static void vsthost_update_editor(t_vsthost *x){
    if (!x->x_plugin->hasEditor() || x->x_generic){
        int n = x->x_plugin->getNumParameters();
        for (int i = 0; i < n; ++i){
            float f = x->x_plugin->getParameter(i);
            t_symbol *param = x->x_param_vec[i].p_name;
            pd_vmess(param->s_thing, gensym("set"), (char *)"f", f);
        }
    }
}

static void vsthost_make_editor(t_vsthost *x){
    int nparams = x->x_plugin->getNumParameters();
    int n = x->x_nparams;
    for (int i = 0; i < n; ++i){
        vstparam_free(&x->x_param_vec[i]);
    }
    x->x_param_vec = (t_vstparam *)resizebytes(x->x_param_vec, n * sizeof(t_vstparam), nparams * sizeof(t_vstparam));
    x->x_nparams = nparams;
    for (int i = 0; i < nparams; ++i){
        vstparam_init(&x->x_param_vec[i], x, i);
    }

    editor_vmess(x, gensym("rename"), (char *)"s", gensym(x->x_plugin->getPluginName().c_str()));
    editor_mess(x, gensym("clear"));
    // slider
    t_atom slider[21];
    SETFLOAT(slider, 20);
    SETFLOAT(slider+1, 20);
    SETSYMBOL(slider+2, gensym("hsl"));
    SETFLOAT(slider+3, 128);
    SETFLOAT(slider+4, 15);
    SETFLOAT(slider+5, 0);
    SETFLOAT(slider+6, 1);
    SETFLOAT(slider+7, 0);
    SETFLOAT(slider+8, 0);
    SETSYMBOL(slider+9, gensym("empty"));
    SETSYMBOL(slider+10, gensym("empty"));
    SETSYMBOL(slider+11, gensym("dummy"));
    SETFLOAT(slider+12, -2);
    SETFLOAT(slider+13, -8);
    SETFLOAT(slider+14, 0);
    SETFLOAT(slider+15, 10);
    SETFLOAT(slider+16, -262144);
    SETFLOAT(slider+17, -1);
    SETFLOAT(slider+18, -1);
    SETFLOAT(slider+19, -0);
    SETFLOAT(slider+20, 1);
    // label
    t_atom label[16];
    SETFLOAT(label, 30 + 128);
    SETFLOAT(label+1, 10);
    SETSYMBOL(label+2, gensym("cnv"));
    SETFLOAT(label+3, 1);
    SETFLOAT(label+4, 1);
    SETFLOAT(label+5, 1);
    SETSYMBOL(label+6, gensym("empty"));
    SETSYMBOL(label+7, gensym("empty"));
    SETSYMBOL(label+8, gensym("empty"));
    SETFLOAT(label+9, 0);
    SETFLOAT(label+10, 8);
    SETFLOAT(label+11, 0);
    SETFLOAT(label+12, 10);
    SETFLOAT(label+13, -262144);
    SETFLOAT(label+14, -66577);
    SETFLOAT(label+15, 0);

    for (int i = 0; i < nparams; ++i){
        // create slider
        SETFLOAT(slider+1, 20 + i*35);
        SETSYMBOL(slider+9, x->x_param_vec[i].p_name);
        SETSYMBOL(slider+10, x->x_param_vec[i].p_name);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d: %s", i, x->x_plugin->getParameterName(i).c_str());
        SETSYMBOL(slider+11, gensym(buf));
        editor_mess(x, gensym("obj"), 21, slider);
        // create number box
        SETFLOAT(label+1, 20 + i*35);
        SETSYMBOL(label+6, x->x_param_vec[i].p_display);
        SETSYMBOL(label+7, x->x_param_vec[i].p_display);
        editor_mess(x, gensym("obj"), 16, label);
    }
    float width = 280;
    float height = nparams * 35 + 60;
    editor_vmess(x, gensym("setbounds"), "ffff", 0.f, 0.f, width, height);
    editor_vmess(x, gensym("vis"), "i", 0);

    vsthost_update_editor(x);
}

static void vsthost_vis(t_vsthost *x, t_floatarg f){
	if (!vsthost_check(x)) return;
    if (x->x_plugin->hasEditor() && !x->x_generic){
        if (f != 0){
            x->x_plugin->showEditorWindow();
        } else {
            x->x_plugin->hideEditorWindow();
        }
    } else {
        editor_vmess(x, gensym("vis"), "i", (f != 0));
    }
}

static void vsthost_click(t_vsthost *x){
    vsthost_vis(x, 1);
}

static void vsthost_generic(t_vsthost *x, t_floatarg f){
    x->x_generic = (f != 0);
}

static void vsthost_precision(t_vsthost *x, t_floatarg f){
    x->x_dp = (f != 0);
}

// parameters
static void vsthost_param_set(t_vsthost *x, t_floatarg _index, t_floatarg value){
	if (!vsthost_check(x)) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
        x->x_plugin->setParameter(index, value);
        if (!x->x_plugin->hasEditor() || x->x_generic){
            vstparam_set(&x->x_param_vec[index], value);
        }
	} else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
	}
}

static void vsthost_param_get(t_vsthost *x, t_floatarg _index){
	if (!vsthost_check(x)) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
		t_atom msg[2];
		SETFLOAT(&msg[0], index);
        SETFLOAT(&msg[1], x->x_plugin->getParameter(index));
		outlet_anything(x->x_messout, gensym("param_value"), 2, msg);
	} else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
	}
}

static void vsthost_param_getname(t_vsthost *x, t_floatarg _index){
	if (!vsthost_check(x)) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
		t_atom msg[2];
		SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getParameterName(index).c_str()));
		outlet_anything(x->x_messout, gensym("param_name"), 2, msg);
	} else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
	}
}

static void vsthost_param_getlabel(t_vsthost *x, t_floatarg _index){
    if (!vsthost_check(x)) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
        t_atom msg[2];
        SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getParameterLabel(index).c_str()));
        outlet_anything(x->x_messout, gensym("param_label"), 2, msg);
    } else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
    }
}

static void vsthost_param_getdisplay(t_vsthost *x, t_floatarg _index){
    if (!vsthost_check(x)) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
        t_atom msg[2];
        SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getParameterDisplay(index).c_str()));
        outlet_anything(x->x_messout, gensym("param_display"), 2, msg);
    } else {
        pd_error(x, "%s: parameter index %d out of range!", classname(x), index);
    }
}

static void vsthost_param_count(t_vsthost *x){
	if (!vsthost_check(x)) return;
	t_atom msg;
    SETFLOAT(&msg, x->x_plugin->getNumParameters());
	outlet_anything(x->x_messout, gensym("param_count"), 1, &msg);
}

static void vsthost_param_list(t_vsthost *x){
	if (!vsthost_check(x)) return;
    int n = x->x_plugin->getNumParameters();
	for (int i = 0; i < n; ++i){
		vsthost_param_getname(x, i);
		vsthost_param_get(x, i);
	}
}

// programs
static void vsthost_program_set(t_vsthost *x, t_floatarg _index){
	if (!vsthost_check(x)) return;
    int index = _index;
    if (index >= 0 && index < x->x_plugin->getNumPrograms()){
        x->x_plugin->setProgram(index);
        vsthost_update_editor(x);
	} else {
        pd_error(x, "%s: program number %d out of range!", classname(x), index);
	}
}

static void vsthost_program_get(t_vsthost *x){
	if (!vsthost_check(x)) return;
	t_atom msg;
    SETFLOAT(&msg, x->x_plugin->getProgram());
    outlet_anything(x->x_messout, gensym("program_number"), 1, &msg);
}

static void vsthost_program_setname(t_vsthost *x, t_symbol* name){
	if (!vsthost_check(x)) return;
    x->x_plugin->setProgramName(name->s_name);
}

static void vsthost_program_getname(t_vsthost *x, t_symbol *s, int argc, t_atom *argv){
	if (!vsthost_check(x)) return;
    t_atom msg[2];
    if (argc){
        int index = atom_getfloat(argv);
        SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getProgramNameIndexed(index).c_str()));
    } else {
        SETFLOAT(&msg[0], x->x_plugin->getProgram());
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getProgramName().c_str()));
    }
    outlet_anything(x->x_messout, gensym("program_name"), 2, msg);
}

static void vsthost_program_count(t_vsthost *x){
	if (!vsthost_check(x)) return;
	t_atom msg;
    SETFLOAT(&msg, x->x_plugin->getNumPrograms());
	outlet_anything(x->x_messout, gensym("program_count"), 1, &msg);
}

static void vsthost_program_list(t_vsthost *x){
#if 0
    if (!vsthost_check(x)) return;
    int old = x->x_plugin->getProgram();
    int n = x->x_plugin->getNumPrograms();
    for (int i = 0; i < n; ++i){
        x->x_plugin->setProgram(i);
        t_atom msg[2];
        SETFLOAT(&msg[0], i);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getProgramName().c_str()));
        outlet_anything(x->x_messout, gensym("program_name"), 2, msg);
    }
    x->x_plugin->setProgram(old);
#endif
    int n = x->x_plugin->getNumPrograms();
    t_atom msg[2];
    for (int i = 0; i < n; ++i){
        SETFLOAT(&msg[0], i);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getProgramNameIndexed(i).c_str()));
        outlet_anything(x->x_messout, gensym("program_name"), 2, msg);
    }
}

// plugin version
static void vsthost_version(t_vsthost *x){
	if (!vsthost_check(x)) return;
    int version = x->x_plugin->getPluginVersion();
	t_atom msg;
	SETFLOAT(&msg, version);
	outlet_anything(x->x_messout, gensym("version"), 1, &msg);
}


/**** private ****/

// constructor
static void *vsthost_new(t_symbol *s, int argc, t_atom *argv){
    t_vsthost *x = (t_vsthost *)pd_new(vsthost_class);
	
    int generic = 0;
    int dp = (PD_FLOATSIZE == 64); // double precision Pd defaults to double precision
    while (argc && argv->a_type == A_SYMBOL){
        const char *flag = atom_getsymbol(argv)->s_name;
        if (*flag == '-'){
            switch (flag[1]){
            case 'g':
                generic = 1;
                break;
            case 'd':
                dp = 1;
                break;
            case 's':
                dp = 0;
                break;
            default:
                break;
            }
            argc--; argv++;
        } else {
            break;
        }
    }
	int in = atom_getfloatarg(0, argc, argv);
	int out = atom_getfloatarg(1, argc, argv);
    if (in < 1) in = 1;
    if (out < 1) out = 1;

    // VST plugin
    x->x_plugin = nullptr;
    x->x_bypass = 0;
    x->x_blocksize = 64;
    x->x_sr = 44100;
    x->x_dp = dp;

    // inputs (skip first):
    for (int i = 1; i < in; ++i){
		inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
	}
    x->x_nin = in;
    x->x_invec = (t_float**)getbytes(sizeof(t_float*) * in);
    // outlets:
	for (int i = 0; i < out; ++i){
		outlet_new(&x->x_obj, &s_signal);
	}
    x->x_nout = out;
    x->x_outvec = (t_float**)getbytes(sizeof(t_float*) * out);

    // Pd editor
    glob_setfilename(0, gensym("VST Plugin Editor"), canvas_getcurrentdir());
    pd_vmess(&pd_canvasmaker, gensym("canvas"), (char *)"siiiii", 0, 0, 100, 100, 10);
    x->x_editor = (t_canvas *)s__X.s_thing;
    editor_vmess(x, gensym("pop"), "i", 0);
    glob_setfilename(0, &s_, &s_);
    x->x_nparams = 0;
    x->x_param_vec = nullptr;
    x->x_generic = generic;

    // buffers
    x->x_inbufsize = 0;
    x->x_inbuf = nullptr;
    x->x_ninbuf = 0;
    x->x_inbufvec = nullptr;
    x->x_outbufsize = 0;
    x->x_outbuf = nullptr;
    x->x_noutbuf = 0;
    x->x_outbufvec = nullptr;

    x->x_f = 0;
    x->x_messout = outlet_new(&x->x_obj, 0);
    return (x);
}

// destructor
static void vsthost_free(t_vsthost *x){
    vsthost_close(x);
    freebytes(x->x_invec, x->x_nin * sizeof(t_float*));
    freebytes(x->x_outvec, x->x_nout * sizeof(t_float*));
    freebytes(x->x_inbuf, x->x_inbufsize);
    freebytes(x->x_outbuf, x->x_outbufsize);
    freebytes(x->x_inbufvec, x->x_ninbuf * sizeof(void*));
    freebytes(x->x_outbufvec, x->x_noutbuf * sizeof(void*));
    if (x->x_param_vec){
        int n = x->x_nparams;
        for (int i = 0; i < n; ++i){
            vstparam_free(&x->x_param_vec[i]);
        }
        freebytes(x->x_param_vec, n * sizeof(t_vstparam));
    }
}

static void vsthost_update_buffer(t_vsthost *x){
    // the input/output buffers must be large enough to fit both
    // the number of Pd inlets/outlets and plugin inputs/outputs.
    // this routine is called in the "dsp" method and when a plugin is loaded.
    int blocksize = x->x_blocksize;
    int nin = x->x_nin;
    int nout = x->x_nout;
    int pin = 0;
    int pout = 0;
    if (x->x_plugin){
        pin = x->x_plugin->getNumInputs();
        pout = x->x_plugin->getNumOutputs();
    }
    int ninbuf = std::max(pin, nin);
    int noutbuf = std::max(pout, nout);
    int inbufsize = ninbuf * sizeof(double) * blocksize;
    int outbufsize = noutbuf * sizeof(double) * blocksize;
    x->x_inbuf = (char*)resizebytes(x->x_inbuf, x->x_inbufsize, inbufsize);
    x->x_outbuf = (char*)resizebytes(x->x_outbuf, x->x_outbufsize, outbufsize);
    x->x_inbufsize = inbufsize;
    x->x_outbufsize = outbufsize;
    x->x_inbufvec = (void**)resizebytes(x->x_inbufvec, x->x_ninbuf * sizeof(void *), ninbuf * sizeof(void *));
    x->x_outbufvec = (void**)resizebytes(x->x_outbufvec, x->x_noutbuf * sizeof(void *), noutbuf * sizeof(void *));
    x->x_ninbuf = ninbuf;
    x->x_noutbuf = noutbuf;
    // post("x_invec: %d, x_inbuf %d, x_inbufvec %d", x->x_invec, x->x_inbuf, x->x_inbufvec);
    // post("x_nin: %d, x_inbufsize %d, x_ninbufvec %d", x->x_nin, x->x_inbufsize, x->x_ninbuf);
}

// perform routine
static t_int *vsthost_perform(t_int *w){
    t_vsthost *x = (t_vsthost *)(w[1]);
    int n = (int)(w[2]);
    auto plugin = x->x_plugin;
    int nin = x->x_nin;
    t_float ** invec = x->x_invec;
    int nout = x->x_nout;
    t_float ** outvec = x->x_outvec;
    int ninbuf = x->x_ninbuf;
    void ** inbufvec = x->x_inbufvec;
    int noutbuf = x->x_noutbuf;
    void ** outbufvec = x->x_outbufvec;
    int out_offset = 0;

    if (plugin && !x->x_bypass){  // process audio
        int pin = plugin->getNumInputs();
        int pout = plugin->getNumOutputs();
        out_offset = pout;
        // process in double precision
        if (x->x_dp && plugin->hasDoublePrecision()){
            // prepare input buffer
            for (int i = 0; i < ninbuf; ++i){
                double *buf = (double *)x->x_inbuf + i * n;
                inbufvec[i] = buf;
                if (i < nin){  // copy from Pd input
                    t_float *in = invec[i];
                    for (int j = 0; j < n; ++j){
                        buf[j] = in[j];
                    }
                } else {  // zero
                    for (int j = 0; j < n; ++j){
                        buf[j] = 0;
                    }
                }
            }
            // set output buffer pointers
            for (int i = 0; i < pout; ++i){
                outbufvec[i] = ((double *)x->x_outbuf + i * n);
            }
            // process
            plugin->processDouble((double **)inbufvec, (double **)outbufvec, n);
            // read from output buffer
            for (int i = 0; i < nout && i < pout; ++i){
                t_float *out = outvec[i];
                double *buf = (double *)outbufvec[i];
                for (int j = 0; j < n; ++j){
                    out[j] = buf[j];
                }
            }
        } else {  // single precision
            // prepare input buffer
            for (int i = 0; i < ninbuf; ++i){
                float *buf = (float *)x->x_inbuf + i * n;
                inbufvec[i] = buf;
                if (i < nin){  // copy from Pd input
                    t_float *in = invec[i];
                    for (int j = 0; j < n; ++j){
                        buf[j] = in[j];
                    }
                } else {  // zero
                    for (int j = 0; j < n; ++j){
                        buf[j] = 0;
                    }
                }
            }
            // set output buffer pointers
            for (int i = 0; i < pout; ++i){
                outbufvec[i] = ((float *)x->x_outbuf + i * n);
            }
            // process
            plugin->process((float **)inbufvec, (float **)outbufvec, n);
            // read from output buffer
            for (int i = 0; i < nout && i < pout; ++i){
                t_float *out = outvec[i];
                float *buf = (float *)outbufvec[i];
                for (int j = 0; j < n; ++j){
                    out[j] = buf[j];
                }
            }
        }
    } else {  // just pass it through
        t_float *buf = (t_float *)x->x_inbuf;
        t_float *bufptr;
        out_offset = nin;
        // copy input
        bufptr = buf;
        for (int i = 0; i < nin && i < nout; ++i){
            t_float *in = invec[i];
            for (int j = 0; j < n; ++j){
                *bufptr++ = in[j];
            }
        }
        // write output
        bufptr = buf;
        for (int i = 0; i < nin && i < nout; ++i){
            t_float *out = outvec[i];
            for (int j = 0; j < n; ++j){
                out[j] = *bufptr++;
            }
        }
    }
    // zero remaining outlets
    for (int i = out_offset; i < nout; ++i){
        auto vec = x->x_outvec[i];
        for (int j = 0; j < n; ++j){
            vec[j] = 0;
        }
    }

    return (w+3);
}

// dsp callback
static void vsthost_dsp(t_vsthost *x, t_signal **sp){
    int blocksize = sp[0]->s_n;
    t_float sr = sp[0]->s_sr;
    dsp_add(vsthost_perform, 2, x, blocksize);
    x->x_blocksize = blocksize;
    x->x_sr = sr;
    if (x->x_plugin){
        x->x_plugin->setBlockSize(blocksize);
        x->x_plugin->setSampleRate(sr);
    }
    int nin = x->x_nin;
    int nout = x->x_nout;
    for (int i = 0; i < nin; ++i){
        x->x_invec[i] = sp[i]->s_vec;
    }
    for (int i = 0; i < nout; ++i){
        x->x_outvec[i] = sp[nin + i]->s_vec;
    }
    vsthost_update_buffer(x);
}

// setup function
extern "C" {
	
void vsthost_tilde_setup(void)
{
    vsthost_class = class_new(gensym("vsthost~"), (t_newmethod)vsthost_new, (t_method)vsthost_free,
        sizeof(t_vsthost), 0, A_GIMME, 0);
    CLASS_MAINSIGNALIN(vsthost_class, t_vsthost, x_f);
    class_addmethod(vsthost_class, (t_method)vsthost_dsp,
        gensym("dsp"), A_CANT, 0);
	class_addmethod(vsthost_class, (t_method)vsthost_open, gensym("open"), A_SYMBOL, 0);
    class_addmethod(vsthost_class, (t_method)vsthost_close, gensym("close"), A_NULL);
	class_addmethod(vsthost_class, (t_method)vsthost_bypass, gensym("bypass"), A_FLOAT);
	class_addmethod(vsthost_class, (t_method)vsthost_vis, gensym("vis"), A_FLOAT, 0);
    class_addmethod(vsthost_class, (t_method)vsthost_click, gensym("click"), A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_precision, gensym("precision"), A_FLOAT, 0);
    class_addmethod(vsthost_class, (t_method)vsthost_generic, gensym("generic"), A_FLOAT, 0);
	// parameters
	class_addmethod(vsthost_class, (t_method)vsthost_param_set, gensym("param_set"), A_FLOAT, A_FLOAT, 0);
	class_addmethod(vsthost_class, (t_method)vsthost_param_get, gensym("param_get"), A_FLOAT, 0);
	class_addmethod(vsthost_class, (t_method)vsthost_param_getname, gensym("param_getname"), A_FLOAT, 0);
    class_addmethod(vsthost_class, (t_method)vsthost_param_getlabel, gensym("param_getlabel"), A_FLOAT, 0);
    class_addmethod(vsthost_class, (t_method)vsthost_param_getdisplay, gensym("param_getdisplay"), A_FLOAT, 0);
	class_addmethod(vsthost_class, (t_method)vsthost_param_count, gensym("param_count"), A_NULL);
	class_addmethod(vsthost_class, (t_method)vsthost_param_list, gensym("param_list"), A_NULL);
	// programs
	class_addmethod(vsthost_class, (t_method)vsthost_program_set, gensym("program_set"), A_FLOAT, 0);
    class_addmethod(vsthost_class, (t_method)vsthost_program_get, gensym("program_get"), A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_program_setname, gensym("program_setname"), A_SYMBOL, 0);
    class_addmethod(vsthost_class, (t_method)vsthost_program_getname, gensym("program_getname"), A_GIMME, 0);
	class_addmethod(vsthost_class, (t_method)vsthost_program_count, gensym("program_count"), A_NULL);
    class_addmethod(vsthost_class, (t_method)vsthost_program_list, gensym("program_list"), A_NULL);
	// version
	class_addmethod(vsthost_class, (t_method)vsthost_version, gensym("version"), A_NULL);

    vstparam_setup();
}

} // extern "C"
