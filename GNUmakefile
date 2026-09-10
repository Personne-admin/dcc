.SUFFIXES:
MAKEFLAGS += --no-builtin-rules --no-builtin-variables

TOPLEVEL := $(abspath .)

CROSS ?=
BUILD_DIR := $(TOPLEVEL)/build$(if $(CROSS),-$(CROSS))
NATIVE_BUILD_DIR := $(TOPLEVEL)/build
OBJ_DIR := $(BUILD_DIR)/obj
PCM_DIR := $(BUILD_DIR)/pcm
DEP_DIR := $(BUILD_DIR)/dep
CONFIG_MK := $(TOPLEVEL)/mk/config.mk
RULES_MK := $(TOPLEVEL)/mk/rules.mk
MODULES_MK := $(TOPLEVEL)/mk/modules.mk
STD_MK := $(TOPLEVEL)/mk/std.mk
COMPDB_MK := $(TOPLEVEL)/mk/compdb.mk
SCAN_SCRIPT := $(TOPLEVEL)/mk/scan_modules.py

COMPDB_SUBDIRS := compiler driver dccd libdcext

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

export TOPLEVEL BUILD_DIR OBJ_DIR PCM_DIR DEP_DIR NATIVE_BUILD_DIR PREFIX DESTDIR CROSS
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

.PHONY: all compiler driver dccd libdcext test install uninstall compdb clean distclean help tools-windows msi

all: driver libdcext dccd

compiler:
	@$(MAKE) -C compiler

driver: compiler
	@$(MAKE) -C driver

dccd: compiler
	@$(MAKE) -C dccd

libdcext: driver
	@$(MAKE) -C libdcext

tools-windows:
	@$(MAKE) CROSS=windows driver
	@$(MAKE) CROSS=windows dccd

MSI_VERSION ?= 0.2.0
MSI_STAGE_DIR := $(TOPLEVEL)/build-windows/msi-stage
MSI_OUT := $(TOPLEVEL)/build-windows/dcc-$(MSI_VERSION)-x86_64.msi
GEN_WXS_SCRIPT := $(TOPLEVEL)/mk/gen_wxs.py

msi: tools-windows
	@$(MAKE) libdcext TARGET=x86_64-windows BACKEND=llvm
	@$(MAKE) libdcext TARGET=x86_64-windows BACKEND=em64t
	@rm -rf $(MSI_STAGE_DIR)
	@mkdir -p $(MSI_STAGE_DIR)/bin $(MSI_STAGE_DIR)/lib
	$(Q)cp $(TOPLEVEL)/build-windows/bin/dcc.exe $(MSI_STAGE_DIR)/bin/
	$(Q)cp $(TOPLEVEL)/build-windows/bin/dccd.exe $(MSI_STAGE_DIR)/bin/
	$(Q)cp $(NATIVE_BUILD_DIR)/lib/libdcext-windows-llvm.a $(MSI_STAGE_DIR)/lib/
	$(Q)cp $(NATIVE_BUILD_DIR)/lib/libdcext-windows-em64t.a $(MSI_STAGE_DIR)/lib/
	$(Q)cp -r $(NATIVE_BUILD_DIR)/include $(MSI_STAGE_DIR)/include
	$(Q)cp $(TOPLEVEL)/LICENSE $(MSI_STAGE_DIR)/
	@mkdir -p $(dir $(MSI_OUT))
	$(Q)$(PYTHON) $(GEN_WXS_SCRIPT) --stage-dir $(MSI_STAGE_DIR) --version $(MSI_VERSION) \
		--output $(TOPLEVEL)/build-windows/dcc.wxs

	$(Q)wixl -v -a x64 -o $(MSI_OUT) $(TOPLEVEL)/build-windows/dcc.wxs
	$(call MSG,MSI,$(MSI_OUT))

test: compiler driver libdcext dccd
	@$(MAKE) libdcext TARGET=x86_64-linux BACKEND=em64t
	@$(MAKE) -C tests

.PHONY: test-linux test-win
test-linux: compiler driver dccd
	@$(MAKE) libdcext TARGET=x86_64-linux BACKEND=llvm
	@$(MAKE) libdcext TARGET=x86_64-linux BACKEND=em64t
	@$(MAKE) -C tests test-linux

test-win: compiler driver dccd
	@$(MAKE) libdcext TARGET=x86_64-windows BACKEND=llvm
	@$(MAKE) libdcext TARGET=x86_64-windows BACKEND=em64t
	@$(MAKE) -C tests test-win

install: driver dccd libdcext
	$(call MSG,INSTALL,$(DESTDIR)$(BINDIR)/dcc)
	$(Q)$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(Q)$(INSTALL) -m 755 $(BUILD_DIR)/bin/dcc $(DESTDIR)$(BINDIR)/dcc

	$(call MSG,INSTALL,$(DESTDIR)$(BINDIR)/dccd)
	$(Q)$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(Q)$(INSTALL) -m 755 $(BUILD_DIR)/bin/dccd $(DESTDIR)$(BINDIR)/dccd

	$(call MSG,INSTALL,$(DESTDIR)$(LIBDIR)/libdcext-*.a)
	$(Q)$(INSTALL) -d $(DESTDIR)$(LIBDIR)
	$(Q)$(INSTALL) -m 644 $(BUILD_DIR)/lib/libdcext-*.a $(DESTDIR)$(LIBDIR)/
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
	rm -f $(DESTDIR)$(LIBDIR)/libdcext-*.a
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
	rm -rf $(BUILD_DIR) $(TOPLEVEL)/build-windows

help:
	@echo "dcc build system"
	@echo ""
	@echo "Targets:"
	@echo "  all (default)  Build the compiler, driver, and libdcext"
	@echo "  compiler       Build only the core library"
	@echo "  driver         Build the dcc binary"
	@echo "  libdcext       Build the extended library"
	@echo "  test           Build and run the test suite"
	@echo "  benchmark      Collect correctness-gated compiler benchmarks (BENCH_ARGS=...)"
	@echo "  test-benchmark Check benchmark correctness gating"
	@echo "  test-linux     Run native Linux OS integration tests and ABI audit"
	@echo "  test-win       Run Win64 OS integration tests under Wine and ABI audit"
	@echo "  install        Install to PREFIX (default: /usr/local)"
	@echo "  uninstall      Remove files installed by 'make install'"
	@echo "  tools-windows  Cross-compile dcc/dccd to run on Windows (needs llvm-mingw)"
	@echo "  msi            Package tools-windows + a windows-targeted libdcext as a .msi (needs msitools)"
	@echo "  clean          Remove build artifacts"
	@echo "  distclean      Remove entire build directory"
	@echo ""
	@echo "Variables:"
	@echo "  CROSS=windows  Cross-compile dcc/dccd for Windows (see tools-windows)"
	@echo "  TARGET=...     What OS libdcext targets: x86_64-linux, x86_64-windows, "
	@echo "                 x86_64-freestanding -- independent of CROSS"
	@echo ""

include $(COMPDB_MK)

.PHONY: benchmark
benchmark: driver
	@$(MAKE) libdcext TARGET=x86_64-linux BACKEND=llvm
	@$(MAKE) libdcext TARGET=x86_64-linux BACKEND=em64t
	$(Q)$(PYTHON) $(TOPLEVEL)/mk/benchmark.py $(BENCH_ARGS)

.PHONY: test-benchmark
test-benchmark:
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/benchmark_runner.py
