.PHONY: all clean install

all: ${PROBE_EXE}

clean:
	rm -f ../vst/${PROBE}.o ${PROBE_EXE}
	rm -f -r *.dSYM

install:
	$(INSTALL_DIR) -v "$(installpath)"
	$(INSTALL_PROGRAM) '${PROBE_EXE}' "$(installpath)"

install-strip: stripflags := --strip-program=$(STRIP) -s
install-strip: install

%.o: %.cpp ${PROBE_DEPS}
	$(CXX) $(cxx.flags) -o $@ -c $<

${PROBE_EXE}: ${PROBE_SRC}
	$(CXX) $(cxx.flags) -o $@ $^ ${PROBE_LDLIBS}

