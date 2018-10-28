#include "VSTPlugin.h"

#include "m_pd.h"

#include <memory>

// Pd wrapper

static t_class *vsthost_class;

struct t_vsthost {
	// header
	t_object x_obj;
	t_sample x_f;
	t_outlet *x_messout;

    IVSTPlugin* x_plugin;
    int x_bypass;
    int x_blocksize;
    int x_sr;
    // input
    int x_nin;
    t_float **x_invec;
    int x_inbufsize;
    char *x_inbuf;
    int x_ninbuf;
    void **x_inbufvec;
    // output
    int x_nout;
    t_float **x_outvec;
    int x_outbufsize;
    char *x_outbuf;
    int x_noutbuf;
    void **x_outbufvec;
};

/**** public interface ****/

// helper functions
static bool vsthost_check(t_vsthost *x){
    if (x->x_plugin){
		return true;
	} else {
		pd_error(x, "no plugin loaded!");
		return false;
	}
}

// close
static void vsthost_close(t_vsthost *x){
    freeVSTPlugin(x->x_plugin);
    x->x_plugin = nullptr;
}

// open

static void vsthost_updatebuffer(t_vsthost *x);

static void vsthost_open(t_vsthost *x, t_symbol *s){
    vsthost_close(x);
    IVSTPlugin *plugin = loadVSTPlugin(s->s_name);
    if (plugin){
        plugin->setBlockSize(x->x_blocksize);
        plugin->setSampleRate(x->x_sr);
        plugin->resume();
        if (plugin->hasSinglePrecision()){
            post("plugin supports single precision");
        }
        if (plugin->hasDoublePrecision()){
            post("plugin supports double precision");
        }
        x->x_plugin = plugin;
        vsthost_updatebuffer(x);
    } else {
        pd_error(x, "couldn't open plugin %s", s->s_name);
    }
}

// bypass
static void vsthost_bypass(t_vsthost *x, t_floatarg f){
    x->x_bypass = (f != 0);
}

// editor
static void vsthost_vis(t_vsthost *x, t_floatarg f){
	if (!vsthost_check(x)) return;
    if (f != 0){
        x->x_plugin->createEditorWindow();
    } else {
        x->x_plugin->destroyEditorWindow();
    }
}

// parameters
static void vsthost_param_set(t_vsthost *x, t_floatarg index, t_floatarg value){
	if (!vsthost_check(x)) return;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
        x->x_plugin->setParameter(index, value);
	} else {
		pd_error(x, "parameter index out of range!");
	}
}

static void vsthost_param_get(t_vsthost *x, t_floatarg index){
	if (!vsthost_check(x)) return;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
		t_atom msg[2];
		SETFLOAT(&msg[0], index);
        SETFLOAT(&msg[1], x->x_plugin->getParameter(index));
		outlet_anything(x->x_messout, gensym("param_value"), 2, msg);
	} else {
		pd_error(x, "parameter index out of range!");
	}
}

static void vsthost_param_getname(t_vsthost *x, t_floatarg index){
	if (!vsthost_check(x)) return;
    if (index >= 0 && index < x->x_plugin->getNumParameters()){
		t_atom msg[2];
		SETFLOAT(&msg[0], index);
        SETSYMBOL(&msg[1], gensym(x->x_plugin->getParameterName(index).c_str()));
		outlet_anything(x->x_messout, gensym("param_name"), 2, msg);
	} else {
		pd_error(x, "parameter index out of range!");
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
static void vsthost_program_set(t_vsthost *x, t_floatarg number){
	if (!vsthost_check(x)) return;
    if (number >= 0 && number < x->x_plugin->getNumParameters()){
        x->x_plugin->setProgram(number);
	} else {
		pd_error(x, "program number out of range!");
	}
}

static void vsthost_program_get(t_vsthost *x){
	if (!vsthost_check(x)) return;
	t_atom msg;
    SETFLOAT(&msg, x->x_plugin->getProgram());
	outlet_anything(x->x_messout, gensym("program_num"), 1, &msg);
}

static void vsthost_program_setname(t_vsthost *x, t_symbol* name){
	if (!vsthost_check(x)) return;
    x->x_plugin->setProgramName(name->s_name);
}

static void vsthost_program_getname(t_vsthost *x){
	if (!vsthost_check(x)) return;
	t_atom msg;
    SETSYMBOL(&msg, gensym(x->x_plugin->getProgramName().c_str()));
	outlet_anything(x->x_messout, gensym("program_name"), 1, &msg);
}

static void vsthost_program_count(t_vsthost *x){
	if (!vsthost_check(x)) return;
	t_atom msg;
    SETFLOAT(&msg, x->x_plugin->getNumPrograms());
	outlet_anything(x->x_messout, gensym("program_count"), 1, &msg);
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
	
	int in = atom_getfloatarg(0, argc, argv);
	int out = atom_getfloatarg(1, argc, argv);
    if (in < 1) in = 1;
    if (out < 1) out = 1;
	for (int i = 0; i < in - 1; ++i){
		inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
	}
	for (int i = 0; i < out; ++i){
		outlet_new(&x->x_obj, &s_signal);
	}
	post("in: %d, out: %d", in, out);
	
    x->x_plugin = nullptr;
    x->x_bypass = 0;
    x->x_blocksize = 0;
    x->x_sr = 0;
    x->x_nin = in;
    x->x_invec = (t_float**)getbytes(sizeof(t_float*) * in);
    x->x_inbufsize = in*sizeof(double)*64;
    x->x_inbuf = (char *)getbytes(x->x_inbufsize);
    x->x_ninbuf = in;
    x->x_inbufvec = (void**)getbytes(in * sizeof(void *));
    x->x_nout = out;
    x->x_outvec = (t_float**)getbytes(sizeof(t_float*) * out);
    x->x_outbufsize = out*sizeof(double)*64;
    x->x_outbuf = (char *)getbytes(x->x_outbufsize);
    x->x_noutbuf = out;
    x->x_outbufvec = (void **)getbytes(out * sizeof(void *));
	x->x_messout = outlet_new(&x->x_obj, 0); // message outlet
    return (x);
}

// destructor
static void vsthost_free(t_vsthost *x){
    vsthost_close(x);
    freebytes(x->x_invec, sizeof(t_float*) * x->x_nin);
    freebytes(x->x_outvec, sizeof(t_float*) * x->x_nout);
    freebytes(x->x_inbuf, sizeof(double) * x->x_nin * x->x_blocksize);
    freebytes(x->x_outbuf, sizeof(double) * x->x_nout * x->x_blocksize);
}

static void vsthost_updatebuffer(t_vsthost *x){
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
    resizebytes(x->x_inbuf, x->x_inbufsize, inbufsize);
    resizebytes(x->x_outbuf, x->x_outbufsize, outbufsize);
    x->x_inbufsize = inbufsize;
    x->x_outbufsize = outbufsize;
    resizebytes(x->x_inbufvec, x->x_ninbuf * sizeof(void *), ninbuf * sizeof(void *));
    resizebytes(x->x_outbufvec, x->x_noutbuf * sizeof(void *), noutbuf * sizeof(void *));
    x->x_ninbuf = ninbuf;
    x->x_noutbuf = noutbuf;
    post("x_invec: %d, x_inbuf %d, x_inbufvec %d", x->x_invec, x->x_inbuf, x->x_inbufvec);
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
    void ** inbufvec = x->x_inbufvec;
    void ** outbufvec = x->x_outbufvec;
    int offset = 0;

    if (plugin && !x->x_bypass){
        int pin = plugin->getNumInputs();
        int pout = plugin->getNumOutputs();
        offset = pout;
        if (plugin->hasDoublePrecision()){
            // set buffer pointer
            for (int i = 0; i < pin; ++i){
                inbufvec[i] = ((double *)x->x_inbuf + i * n);
            }
            for (int i = 0; i < pout; ++i){
                outbufvec[i] = ((double *)x->x_outbuf + i * n);
            }
            // fill input buffer
            for (int i = 0; i < nin && i < pin; ++i){
                t_float *in = invec[i];
                double *buf = (double *)inbufvec[i];
                for (int j = 0; j < n; ++j){
                    buf[j] = in[j];
                }
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
            // set buffer pointer
            for (int i = 0; i < pin; ++i){
                inbufvec[i] = ((float *)x->x_inbuf + i * n);
            }
            for (int i = 0; i < pout; ++i){
                outbufvec[i] = ((float *)x->x_outbuf + i * n);
            }
            // fill input buffer
            for (int i = 0; i < nin && i < pin; ++i){
                t_float *in = invec[i];
                float *buf = (float *)inbufvec[i];
                for (int j = 0; j < n; ++j){
                    buf[j] = in[j];
                }
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
    } else {
        t_float *bufptr = (t_float *)x->x_inbuf;
        offset = nin;
        // copy input
        for (int i = 0; i < nin && i < nout; ++i){
            t_float *in = invec[i];
            t_float *buf = bufptr + i * n;
            for (int j = 0; j < n; ++j){
                buf[j] = in[j];
            }
        }
        // write output
        for (int i = 0; i < nin && i < nout; ++i){
            t_float *out = outvec[i];
            t_float *buf = bufptr + i * n;
            for (int j = 0; j < n; ++j){
                out[j] = buf[j];
            }
        }
    }
    // zero remaining outlets
    for (int i = offset; i < nout; ++i){
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
    vsthost_updatebuffer(x);
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
	// parameters
	class_addmethod(vsthost_class, (t_method)vsthost_param_set, gensym("param_set"), A_FLOAT, A_FLOAT, 0);
	class_addmethod(vsthost_class, (t_method)vsthost_param_get, gensym("param_get"), A_FLOAT, 0);
	class_addmethod(vsthost_class, (t_method)vsthost_param_getname, gensym("param_getname"), A_FLOAT, 0);
	class_addmethod(vsthost_class, (t_method)vsthost_param_count, gensym("param_count"), A_NULL);
	class_addmethod(vsthost_class, (t_method)vsthost_param_list, gensym("param_list"), A_NULL);
	// programs
	class_addmethod(vsthost_class, (t_method)vsthost_program_set, gensym("program_set"), A_FLOAT, 0);
	class_addmethod(vsthost_class, (t_method)vsthost_program_get, gensym("program_get"), A_NULL);
	class_addmethod(vsthost_class, (t_method)vsthost_program_setname, gensym("program_setname"), A_SYMBOL, 0);
	class_addmethod(vsthost_class, (t_method)vsthost_program_getname, gensym("program_getname"), A_NULL);
	class_addmethod(vsthost_class, (t_method)vsthost_program_count, gensym("program_count"), A_NULL);
	// version
	class_addmethod(vsthost_class, (t_method)vsthost_version, gensym("version"), A_NULL);
}

} // extern "C"
