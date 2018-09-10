#include "vsthost.h"

// Pd wrapper

static t_class *vsthost_class;

struct t_vsthost {
	// header
	t_object x_obj;
	t_sample x_f;
	t_outlet *x_messout;
	VSTHost *x_host;
};

/**** public interface ****/

// helper functions
static bool vsthost_check(t_vsthost *x){
	if (x->x_host->hasPlugin()){
		return true;
	} else {
		pd_error(x, "no plugin loaded!");
		return false;
	}
}

// open
static void vsthost_open(t_vsthost *x, t_symbol *s){
	x->x_host->openPlugin(s->s_name);
}

// bypass
static void vsthost_bypass(t_vsthost *x, t_floatarg f){
	if (!vsthost_check(x)) return;
	x->x_host->setBypass(f);
}

// editor
static void vsthost_vis(t_vsthost *x, t_floatarg f){
	if (!vsthost_check(x)) return;
	if (f != 0)
		x->x_host->showEditor();
	else
		x->x_host->hideEditor();
}

// parameters
static void vsthost_param_set(t_vsthost *x, t_floatarg index, t_floatarg value){
	if (!vsthost_check(x)) return;
	if (index >= 0 && index < x->x_host->getNumParameters()){
		x->x_host->setParameter(index, value);
	} else {
		pd_error(x, "parameter index out of range!");
	}
}

static void vsthost_param_get(t_vsthost *x, t_floatarg index){
	if (!vsthost_check(x)) return;
	if (index >= 0 && index < x->x_host->getNumParameters()){
		t_atom msg[2];
		SETFLOAT(&msg[0], index);
		SETFLOAT(&msg[1], x->x_host->getParameter(index));
		outlet_anything(x->x_messout, gensym("param_value"), 2, msg);
	} else {
		pd_error(x, "parameter index out of range!");
	}
}

static void vsthost_param_getname(t_vsthost *x, t_floatarg index){
	if (!vsthost_check(x)) return;
	if (index >= 0 && index < x->x_host->getNumParameters()){
		t_atom msg[2];
		SETFLOAT(&msg[0], index);
		SETSYMBOL(&msg[1], gensym(x->x_host->getParameterName(index).c_str()));
		outlet_anything(x->x_messout, gensym("param_name"), 2, msg);
	} else {
		pd_error(x, "parameter index out of range!");
	}
}

static void vsthost_param_count(t_vsthost *x){
	if (!vsthost_check(x)) return;
	t_atom msg;
	SETFLOAT(&msg, x->x_host->getNumParameters());
	outlet_anything(x->x_messout, gensym("param_count"), 1, &msg);
}

static void vsthost_param_list(t_vsthost *x){
	if (!vsthost_check(x)) return;
	int n = x->x_host->getNumParameters();
	for (int i = 0; i < n; ++i){
		vsthost_param_getname(x, i);
		vsthost_param_get(x, i);
	}
}

// programs
static void vsthost_program_set(t_vsthost *x, t_floatarg number){
	if (!vsthost_check(x)) return;
	if (number >= 0 && number < x->x_host->getNumParameters()){
		x->x_host->setProgram(number);
	} else {
		pd_error(x, "program number out of range!");
	}
}

static void vsthost_program_get(t_vsthost *x){
	if (!vsthost_check(x)) return;
	t_atom msg;
	SETFLOAT(&msg, x->x_host->getProgram());
	outlet_anything(x->x_messout, gensym("program_num"), 1, &msg);
}

static void vsthost_program_setname(t_vsthost *x, t_symbol* name){
	if (!vsthost_check(x)) return;
	x->x_host->setProgramName(name->s_name);
}

static void vsthost_program_getname(t_vsthost *x){
	if (!vsthost_check(x)) return;
	t_atom msg;
	SETSYMBOL(&msg, gensym(x->x_host->getProgramName().c_str()));
	outlet_anything(x->x_messout, gensym("program_name"), 1, &msg);
}

static void vsthost_program_count(t_vsthost *x){
	if (!vsthost_check(x)) return;
	t_atom msg;
	SETFLOAT(&msg, x->x_host->getNumPrograms());
	outlet_anything(x->x_messout, gensym("program_count"), 1, &msg);
}

// plugin version
static void vsthost_version(t_vsthost *x){
	if (!vsthost_check(x)) return;
	int version = x->x_host->getVstVersion();
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
	if (in < 2) in = 2;
	if (out < 2) out = 2;
	for (int i = 0; i < in - 1; ++i){
		inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
	}
	for (int i = 0; i < out; ++i){
		outlet_new(&x->x_obj, &s_signal);
	}
	post("in: %d, out: %d", in, out);
	
	x->x_host = new VSTHost(in, out);
	
	x->x_messout = outlet_new(&x->x_obj, 0); // message outlet
    return (x);
}

// destructor
static void vsthost_free(t_vsthost *x){
	delete x->x_host;
}

// perform routine
static t_int *vsthost_perform(t_int *w){
    VSTHost *x = (VSTHost *)(w[1]);
    int n = (t_int)(w[2]);
	x->perform(n);
    return (w+3);
}

// dsp callback
static void vsthost_dsp(t_vsthost *x, t_signal **sp){
	auto host = x->x_host;
    dsp_add(vsthost_perform, 2, host, sp[0]->s_n);
	
	host->setBlockSize(sp[0]->s_n);
	host->setSampleRate(sp[0]->s_sr);
	
	int k = 0;
	for (int i = 0; i < host->getNumHostInputs(); ++i, ++k){
		host->setInputBuffer(i, sp[k]->s_vec);
	}
	for (int i = 0; i < host->getNumHostOutputs(); ++i, ++k){
		host->setOutputBuffer(i, sp[k]->s_vec);
	}
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