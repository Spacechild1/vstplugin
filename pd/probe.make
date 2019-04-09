.PHONY: all clean install install-strip

PROBE_OBJ = $(addsuffix .o,$(basename $(PROBE_SRC)))

all: $(PROBE_EXE)

clean:
	rm -f $(PROBE_OBJ) $(PROBE_EXE)
	rm -f -r *.dSYM

install: all
	$(INSTALL_DIR) -v "$(installpath)"
	$(INSTALL) $(stripflags) -p -m 755 '$(PROBE_EXE)' "$(installpath)"

install-strip: stripflags := --strip-program=$(STRIP) -s
install-strip: install

%.o: %.cpp $(PROBE_DEPS)
	$(CXX) $(cxx.flags) -o $@ -c $<

%.o: %.mm $(PROBE_DEPS)
	$(CXX) $(cxx.flags) -o $@ -c $<

$(PROBE_EXE): $(PROBE_OBJ)
	$(CXX) $(cxx.flags) -o $@ $^ $(PROBE_LDLIBS)
