.DEFAULT_GOAL := all
include $(RULES_MK)

LOCAL_CXXFLAGS := $(LLVM_DEFS) $(LLVM_CXXFLAGS)

.PHONY: all test test-lsp-diagnostics test-i8086 clean

COMPILER_LIB := $(BUILD_DIR)/lib/libcompiler.a

ALL_TEST_SRCS := $(filter-out %/test_stdlib_os_integration.cc,$(call find-sources,$(TOPLEVEL)/tests/source,cc))
OS_TEST_OBJ := $(OBJ_DIR)/tests/source/test_stdlib_os_integration.cc.o

MINGW_SYSROOT ?= /opt/llvm-mingw
WINE ?= $(shell command -v wine64 2>/dev/null || command -v wine 2>/dev/null)

.PHONY: test-linux test-win
test-linux: $(BUILD_DIR)/bin/tests/test_stdlib_os_integration
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/stdlib/check_abi.py linux
	$(Q)$<
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/abi/cross_backend.py linux --build $(BUILD_DIR)
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/abi/unwind.py
test-win: $(BUILD_DIR)/bin/tests/test_stdlib_os_integration
	$(Q)WINEDEBUG=-all WINE="$(WINE)" MINGW_SYSROOT="$(MINGW_SYSROOT)" $(PYTHON) $(TOPLEVEL)/tests/backend/test_memory.py $(BUILD_DIR)/bin/dcc --windows
	$(Q)MINGW_SYSROOT="$(MINGW_SYSROOT)" $(PYTHON) $(TOPLEVEL)/tests/stdlib/check_abi.py win
	$(Q)DCC_TEST_WINDOWS=1 WINEDEBUG=-all WINE="$(WINE)" MINGW_SYSROOT="$(MINGW_SYSROOT)" $<
	$(Q)WINEDEBUG=-all WINE="$(WINE)" MINGW_SYSROOT="$(MINGW_SYSROOT)" $(PYTHON) $(TOPLEVEL)/tests/abi/cross_backend.py win --build $(BUILD_DIR)

ifeq ($(ENABLE_LLVM),0)
  TEST_SRCS := $(filter-out %/test_backend.cc,$(ALL_TEST_SRCS))
else
  TEST_SRCS := $(ALL_TEST_SRCS)
endif

ALL_TEST_OBJS := $(call objs-from-srcs,$(TEST_SRCS))
ALL_TEST_BINS := $(patsubst $(TOPLEVEL)/tests/source/%.cc,$(BUILD_DIR)/bin/tests/%,$(TEST_SRCS))
ALL_DEPS := $(ALL_TEST_OBJS:.o=.d) $(OS_TEST_OBJ:.o=.d)

COMPILER_PCMS := $(wildcard $(PCM_DIR)/dcc.*.pcm)
DCCD_PCMS := $(wildcard $(PCM_DIR)/dccd.*.pcm)

$(OS_TEST_OBJ): $(TOPLEVEL)/tests/source/test_stdlib_os_integration.cc $(COMPILER_PCMS)
	$(call compile-cxx,$<,$@,$(LOCAL_CXXFLAGS))

TEST_LDFLAGS := $(LLVM_LDFLAGS) $(CMARK_LIBS)

$(OBJ_DIR)/tests/source/test_driver_prefix.cc.o: LOCAL_CXXFLAGS += -DDCC_EXPECTED_VERSION='"$(DCC_VERSION)"'
$(OBJ_DIR)/tests/source/test_driver_prefix.cc.o: $(VERSION_STAMP)

$(OBJ_DIR)/tests/source/test_x86_real16.cc.o: LOCAL_CXXFLAGS += -DDCC_I8086_RUNNER='"$(TOPLEVEL)/tests/i8086/run.py"'
$(OBJ_DIR)/tests/source/test_cases_dispatcher.cc.o: LOCAL_CXXFLAGS += -DDCC_I8086_RUNNER='"$(TOPLEVEL)/tests/i8086/run.py"' -DDCC_I8086_CRT0='"$(TOPLEVEL)/tests/i8086/crt0.asm"' -DDCC_I8086_DIFFERENTIAL='"$(TOPLEVEL)/tests/i8086/differential.list"'

DCCD_FORMAT_OBJ := $(OBJ_DIR)/dccd.format.pcm.o
DCCD_PROTOCOL_OBJ := $(OBJ_DIR)/dccd.protocol.pcm.o
DCCD_WORKSPACE_OBJS := \
	$(OBJ_DIR)/dccd.protocol.pcm.o \
	$(OBJ_DIR)/dccd.workspace_index.pcm.o
DCCD_TRANSPORT_OBJS := \
	$(OBJ_DIR)/dccd.protocol.pcm.o \
	$(OBJ_DIR)/dccd.transport.pcm.o
DCCD_SERVER_OBJS := \
	$(OBJ_DIR)/dccd.protocol.pcm.o \
	$(OBJ_DIR)/dccd.compilation_database.pcm.o \
	$(OBJ_DIR)/dccd.completion.pcm.o \
	$(OBJ_DIR)/dccd.inlay_hints.pcm.o \
	$(OBJ_DIR)/dccd.semantic_tokens.pcm.o \
	$(OBJ_DIR)/dccd.workspace_index.pcm.o \
	$(OBJ_DIR)/dccd.format.pcm.o \
	$(OBJ_DIR)/dccd.transport.pcm.o \
	$(OBJ_DIR)/dccd.server.pcm.o
DCCD_COMPDB_OBJS := \
	$(OBJ_DIR)/dccd.protocol.pcm.o \
	$(OBJ_DIR)/dccd.compilation_database.pcm.o

DCDOC_PCMS := $(wildcard $(PCM_DIR)/dcdoc.*.pcm)
DCDOC_OBJS := \
	$(OBJ_DIR)/dcdoc.model.pcm.o \
	$(OBJ_DIR)/dcdoc.builder.pcm.o \
	$(OBJ_DIR)/dcdoc.prose.pcm.o \
	$(OBJ_DIR)/dcdoc.typst.emit.pcm.o \
	$(OBJ_DIR)/dcdoc.html.emit.pcm.o \
	$(OBJ_DIR)/dcdoc.markdown.emit.pcm.o

all: test

test-i8086:
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/i8086/test_harness.py

test-lsp-diagnostics: $(BUILD_DIR)/bin/dccd
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/lsp_diagnostics.py $(BUILD_DIR)/bin/dccd

test: $(ALL_TEST_BINS) test-lsp-diagnostics
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_i8086_models.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_i8086_link.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_linker_arguments.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_libdcext_abi.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/test_compdb_output.py $(BUILD_DIR)/bin/dcc $(BUILD_DIR)/bin/dccd
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/test_dccd_project_layout.py $(BUILD_DIR)/bin/dcc $(BUILD_DIR)/bin/dccd
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_os_gating.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_freestanding.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_float_gating.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_float_available.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_missing_libdcext.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_memory.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_no_simd.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_archive_members.py
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_domtree_cycles.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_static_match.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_template_instance.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_using_public.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_file_prefix_map.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_libdcext_prefix_map.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_codegen_determinism.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_mem2reg.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/backend/test_debug_info.py $(BUILD_DIR)/bin/dcc
	$(Q)$(PYTHON) $(TOPLEVEL)/tests/i8086/test_harness.py
	@passed=0; failed=0; \
	for bin in $(ALL_TEST_BINS); do \
		name=$$(basename $$bin); \
		if DCC_TEST_LIBDCEXT_SRC="$(TOPLEVEL)/libdcext" DCC_TEST_LIBDCEXT_A="$(BUILD_DIR)/lib/libdcext-linux-custom.a" $$bin; then \
			passed=$$((passed+1)); \
		else \
			failed=$$((failed+1)); \
			printf "  FAIL     %s\n" "$$name"; \
		fi; \
	done; \
	if [ $$failed -eq 0 ]; then \
		printf "  PASS     %d/%d test suites\n" $$passed $$((passed+failed)); \
	else \
		printf "  FAIL     %d/%d test suites\n" $$failed $$((passed+failed)); \
		exit 1; \
	fi

$(BUILD_DIR)/bin/tests/test_format: $(OBJ_DIR)/tests/source/test_format.cc.o $(DCCD_FORMAT_OBJ) $(DCCD_PROTOCOL_OBJ) $(COMPILER_LIB)
	$(call link-binary,$@,$^,$(TEST_LDFLAGS))

$(BUILD_DIR)/bin/tests/test_protocol: $(OBJ_DIR)/tests/source/test_protocol.cc.o $(DCCD_PROTOCOL_OBJ) $(COMPILER_LIB)
	$(call link-binary,$@,$^,$(TEST_LDFLAGS))

$(BUILD_DIR)/bin/tests/test_workspace_index: $(OBJ_DIR)/tests/source/test_workspace_index.cc.o $(DCCD_WORKSPACE_OBJS) $(COMPILER_LIB)
	$(call link-binary,$@,$^,$(TEST_LDFLAGS))

$(BUILD_DIR)/bin/tests/test_transport: $(OBJ_DIR)/tests/source/test_transport.cc.o $(DCCD_TRANSPORT_OBJS) $(COMPILER_LIB)
	$(call link-binary,$@,$^,$(TEST_LDFLAGS))

$(BUILD_DIR)/bin/tests/test_lsp: $(OBJ_DIR)/tests/source/test_lsp.cc.o $(DCCD_SERVER_OBJS) $(COMPILER_LIB)
	$(call link-binary,$@,$^,$(TEST_LDFLAGS))

$(BUILD_DIR)/bin/tests/test_compilation_database: $(OBJ_DIR)/tests/source/test_compilation_database.cc.o $(DCCD_COMPDB_OBJS) $(COMPILER_LIB)
	$(call link-binary,$@,$^,$(TEST_LDFLAGS))

$(BUILD_DIR)/bin/tests/test_dcdoc: $(OBJ_DIR)/tests/source/test_dcdoc.cc.o $(DCDOC_OBJS) $(COMPILER_LIB)
	$(call link-binary,$@,$^,$(TEST_LDFLAGS))

$(BUILD_DIR)/bin/tests/%: $(OBJ_DIR)/tests/source/%.cc.o $(COMPILER_LIB)
	$(call link-binary,$@,$^,$(TEST_LDFLAGS))

$(OBJ_DIR)/tests/source/test_format.cc.o: $(TOPLEVEL)/tests/source/test_format.cc $(COMPILER_PCMS) $(DCCD_PCMS)
	$(call compile-cxx,$<,$@,$(LOCAL_CXXFLAGS))

$(OBJ_DIR)/tests/source/test_protocol.cc.o: $(TOPLEVEL)/tests/source/test_protocol.cc $(COMPILER_PCMS) $(DCCD_PCMS)
	$(call compile-cxx,$<,$@,$(LOCAL_CXXFLAGS))

$(OBJ_DIR)/tests/source/test_workspace_index.cc.o: $(TOPLEVEL)/tests/source/test_workspace_index.cc $(COMPILER_PCMS) $(DCCD_PCMS)
	$(call compile-cxx,$<,$@,$(LOCAL_CXXFLAGS))

$(OBJ_DIR)/tests/source/test_transport.cc.o: $(TOPLEVEL)/tests/source/test_transport.cc $(COMPILER_PCMS) $(DCCD_PCMS)
	$(call compile-cxx,$<,$@,$(LOCAL_CXXFLAGS))

$(OBJ_DIR)/tests/source/test_lsp.cc.o: $(TOPLEVEL)/tests/source/test_lsp.cc $(COMPILER_PCMS) $(DCCD_PCMS)
	$(call compile-cxx,$<,$@,$(LOCAL_CXXFLAGS))

$(OBJ_DIR)/tests/source/test_compilation_database.cc.o: $(TOPLEVEL)/tests/source/test_compilation_database.cc $(COMPILER_PCMS) $(DCCD_PCMS)
	$(call compile-cxx,$<,$@,$(LOCAL_CXXFLAGS))

$(OBJ_DIR)/tests/source/test_dcdoc.cc.o: $(TOPLEVEL)/tests/source/test_dcdoc.cc $(COMPILER_PCMS) $(DCDOC_PCMS)
	$(call compile-cxx,$<,$@,$(LOCAL_CXXFLAGS))

$(filter-out $(OBJ_DIR)/tests/source/test_format.cc.o $(OBJ_DIR)/tests/source/test_protocol.cc.o $(OBJ_DIR)/tests/source/test_lsp.cc.o $(OBJ_DIR)/tests/source/test_workspace_index.cc.o $(OBJ_DIR)/tests/source/test_transport.cc.o $(OBJ_DIR)/tests/source/test_compilation_database.cc.o $(OBJ_DIR)/tests/source/test_dcdoc.cc.o,$(ALL_TEST_OBJS)): $(OBJ_DIR)/%.o: $(TOPLEVEL)/% $(COMPILER_PCMS)
	$(call compile-cxx,$<,$@,$(LOCAL_CXXFLAGS))

clean:
	rm -f $(ALL_TEST_OBJS) $(OS_TEST_OBJ) $(ALL_DEPS) $(ALL_TEST_BINS)
	rm -rf $(BUILD_DIR)/bin/tests

-include $(ALL_DEPS)

include $(STD_MK)
