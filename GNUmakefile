.SUFFIXES:
MAKEFLAGS += --no-builtin-rules --no-builtin-variables

TOPLEVEL := $(abspath .)
BUILD_DIR := $(TOPLEVEL)/build
OBJ_DIR := $(BUILD_DIR)/obj
PCM_DIR := $(BUILD_DIR)/pcm
DEP_DIR := $(BUILD_DIR)/dep
CONFIG_MK := $(TOPLEVEL)/mk/config.mk
RULES_MK := $(TOPLEVEL)/mk/rules.mk
MODULES_MK := $(TOPLEVEL)/mk/modules.mk
STD_MK := $(TOPLEVEL)/mk/std.mk
COMPDB_MK := $(TOPLEVEL)/mk/compdb.mk
SCAN_SCRIPT := $(TOPLEVEL)/mk/scan_modules.py

COMPDB_SUBDIRS := compiler driver dccd

PREFIX ?= /usr/local
DESTDIR ?=
ENABLE_LLVM ?= 1
ENABLE_ASAN ?= 0
BUILD_TYPE ?= debug

BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
DOCDIR ?= $(PREFIX)/share/doc/dcc

-include configure.mk

export TOPLEVEL BUILD_DIR OBJ_DIR PCM_DIR DEP_DIR PREFIX DESTDIR
export ENABLE_LLVM ENABLE_ASAN BUILD_TYPE
export BINDIR LIBDIR INCLUDEDIR DOCDIR
export CONFIG_MK RULES_MK MODULES_MK STD_MK COMPDB_MK SCAN_SCRIPT

include $(CONFIG_MK)

ifndef V
  Q := @
  MSG = @printf "  %-8s %s\n" "$(1)" "$(2)"
else
  Q :=
  MSG = @true
endif

.PHONY: all compiler driver dccd libdcext test install uninstall compdb clean distclean help

all: driver libdcext dccd

compiler:
	@$(MAKE) -C compiler

driver: compiler
	@$(MAKE) -C driver

dccd: compiler
	@$(MAKE) -C dccd

libdcext: driver
	@$(MAKE) -C libdcext

test: compiler driver libdcext dccd
	@$(MAKE) -C tests

install: driver dccd libdcext
	$(call MSG,INSTALL,$(DESTDIR)$(BINDIR)/dcc)
	$(Q)$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(Q)$(INSTALL) -m 755 $(BUILD_DIR)/bin/dcc $(DESTDIR)$(BINDIR)/dcc

	$(call MSG,INSTALL,$(DESTDIR)$(BINDIR)/dccd)
	$(Q)$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(Q)$(INSTALL) -m 755 $(BUILD_DIR)/bin/dccd $(DESTDIR)$(BINDIR)/dccd

	$(call MSG,INSTALL,$(DESTDIR)$(LIBDIR)/libdcext.a)
	$(Q)$(INSTALL) -d $(DESTDIR)$(LIBDIR)
	$(Q)$(INSTALL) -m 644 $(BUILD_DIR)/lib/libdcext.a $(DESTDIR)$(LIBDIR)/libdcext.a
	$(call MSG,INSTALL,$(DESTDIR)$(INCLUDEDIR)/)
	$(Q)$(INSTALL) -d $(DESTDIR)$(INCLUDEDIR)
	$(Q)cp -r $(BUILD_DIR)/include/* $(DESTDIR)$(INCLUDEDIR)/

	$(call MSG,INSTALL,$(DESTDIR)$(DOCDIR)/LICENSE)
	$(Q)$(INSTALL) -d $(DESTDIR)$(DOCDIR)
	$(Q)$(INSTALL) -m 644 $(TOPLEVEL)/LICENSE $(DESTDIR)$(DOCDIR)/LICENSE

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/dcc
	rm -f $(DESTDIR)$(BINDIR)/dccd
	rm -f $(DESTDIR)$(LIBDIR)/libdcext.a
	find $(DESTDIR)$(INCLUDEDIR)/std -name '*.dc' -delete 2>/dev/null || true
	rm -f $(DESTDIR)$(DOCDIR)/LICENSE

compdb:
	@for dir in $(COMPDB_SUBDIRS); do \
		$(MAKE) -C $$dir compdb-fragment; \
	done

	@$(MAKE) compdb-merge

clean:
	@for dir in compiler driver dccd libdcext tests; do \
		$(MAKE) -C $$dir clean 2>/dev/null || true; \
	done

	rm -rf $(OBJ_DIR) $(PCM_DIR) $(DEP_DIR)

distclean:
	rm -rf $(BUILD_DIR)

help:
	@echo "dcc build system"
	@echo ""
	@echo "Targets:"
	@echo "  all (default)  Build the compiler, driver, and libdcext"
	@echo "  compiler       Build only the core library"
	@echo "  driver         Build the dcc binary"
	@echo "  libdcext       Build the extended library"
	@echo "  test           Build and run the test suite"
	@echo "  install        Install to PREFIX (default: /usr/local)"
	@echo "  uninstall      Remove files installed by 'make install'"
	@echo "  clean          Remove build artifacts"
	@echo "  distclean      Remove entire build directory"
	@echo ""

include $(COMPDB_MK)
