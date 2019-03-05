# Windows only

.PHONY: all clean install

all: probe.exe

clean:
	rm -f ../vst/probe.o probe.exe

install:
	$(INSTALL_DIR) -v "$(installpath)"
	$(INSTALL_PROGRAM) 'probe.exe' "$(installpath)"

install-strip: stripflags := --strip-program=$(STRIP) -s
install-strip: install

../vst/probe.o: ../vst/probe.cpp
	$(CXX) $(cxx.flags) -o $@ -c $<

probe.exe: ../vst/probe.o
	$(CXX) $(cxx.flags) -o $@ $^ -static-libgcc -static-libstdc++ -L. -l:vstplugin~.dll

