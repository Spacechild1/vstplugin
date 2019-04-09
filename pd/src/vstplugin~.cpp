// [vstplugin~] is a tiny stub which calls the 'real' setup function in the shared lib.

extern "C" {

void vstplugin_setup(void);

void vstplugin_tilde_setup(void){
    vstplugin_setup();
}

}
