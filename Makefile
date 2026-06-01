# Out-of-tree graphviz layout plugin: `cfg` (control-flow-graph layout).
#
# Builds against a STOCK graphviz install (located via pkg-config) and installs
# into graphviz's plugin directory. No graphviz fork or source tree required.
#
#   make            # build libgvplugin_cfg.<ver>.<ext>
#   make install    # copy into the graphviz plugin dir and run `dot -c`
#   make uninstall  # remove it and re-register
#   make test       # render a sample CFG with -Kcfg

UNAME := $(shell uname)
PLUGIN_VERSION := 6

ifeq ($(UNAME), Darwin)
  LIBEXT      := dylib
  LIBNAME     := libgvplugin_cfg.$(PLUGIN_VERSION).$(LIBEXT)
  SONAME_FLAG := -install_name @rpath/$(LIBNAME)
  SHARED      := -dynamiclib
else
  LIBEXT      := so
  LIBNAME     := libgvplugin_cfg.$(LIBEXT).$(PLUGIN_VERSION)
  SONAME_FLAG := -Wl,-soname,$(LIBNAME)
  SHARED      := -shared
endif

CC      ?= cc
CFLAGS  ?= -O2 -fPIC -Wall
GV_CFLAGS := $(shell pkg-config --cflags libgvc)
GV_LIBS   := $(shell pkg-config --libs libgvc)
PLUGIN_DIR := $(shell pkg-config --variable=libdir libgvc)/graphviz

SRC := cfg_core.c cfg_layout.c gvplugin_cfg.c

all: $(LIBNAME)

$(LIBNAME): $(SRC) cfg_core.h
	$(CC) $(CFLAGS) $(SHARED) -o $@ $(SRC) -I. $(GV_CFLAGS) $(GV_LIBS) $(SONAME_FLAG)

install: $(LIBNAME)
	@echo "Installing into $(PLUGIN_DIR)"
	cp $(LIBNAME) "$(PLUGIN_DIR)/"
	ln -sf $(LIBNAME) "$(PLUGIN_DIR)/libgvplugin_cfg.$(LIBEXT)"
	chmod u+w "$(PLUGIN_DIR)/config6" 2>/dev/null || true
	dot -c
	@echo "Done. Try: dot -Kcfg -Tpng yourgraph.dot -o out.png"

uninstall:
	rm -f "$(PLUGIN_DIR)/$(LIBNAME)" "$(PLUGIN_DIR)/libgvplugin_cfg.$(LIBEXT)"
	chmod u+w "$(PLUGIN_DIR)/config6" 2>/dev/null || true
	dot -c
	@echo "Removed cfg plugin and re-registered."

test: install
	printf 'digraph{node[shape=box];B0->B1;B1->B2[cfgbias=left];B1->B3[cfgbias=right];B2->B1}' \
	  | dot -Kcfg -Tpng -o /tmp/cfg_plugin_test.png && echo "wrote /tmp/cfg_plugin_test.png"

clean:
	rm -f $(LIBNAME) libgvplugin_cfg.$(LIBEXT)

.PHONY: all install uninstall test clean
