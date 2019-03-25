// [vstsearch] is a tiny stub which just calls 'vst_search' when loaded.

extern "C" {

void vst_search(void);

void vstsearch_setup(void){
    vst_search();
}

}
