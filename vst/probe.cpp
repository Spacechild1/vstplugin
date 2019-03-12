extern "C" int probe(const wchar_t *pluginPath, const wchar_t *filePath);

int wmain(int argc, const wchar_t *argv[]){
    if (argc >= 2){
        // plugin path + temp file path (if given)
        return probe(argv[1], argc >= 3 ? argv[2] : nullptr);
    }
    return 0;
}
