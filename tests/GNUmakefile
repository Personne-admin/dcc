include $(RULES_MK)

LOCAL_CXXFLAGS := $(LLVM_DEFS) $(LLVM_CXXFLAGS)

.PHONY: all test clean

COMPILER_LIB := $(BUILD_DIR)/lib/libcompiler.a

ALL_TEST_SRCS := $(call find-sources,$(TOPLEVEL)/tests/source,cc)

ifeq ($(ENABLE_LLVM),0)
  TEST_SRCS := $(filter-out %/test_backend.cc,$(ALL_TEST_SRCS))
else
  TEST_SRCS := $(ALL_TEST_SRCS)
endif

ALL_TEST_OBJS := $(call objs-from-srcs,$(TEST_SRCS))
ALL_TEST_BINS := $(patsubst $(TOPLEVEL)/tests/source/%.cc,$(BUILD_DIR)/bin/tests/%,$(TEST_SRCS))
ALL_DEPS := $(ALL_TEST_OBJS:.o=.d)

COMPILER_PCMS := $(wildcard $(PCM_DIR)/dcc.*.pcm)
DCCD_PCMS := $(wildcard $(PCM_DIR)/dccd.*.pcm)

TEST_LDFLAGS := $(LLVM_LDFLAGS)

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

all: test

test: $(ALL_TEST_BINS)
	@passed=0; failed=0; \
	for bin in $(ALL_TEST_BINS); do \
		name=$$(basename $$bin); \
		if $$bin; then \
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

$(filter-out $(OBJ_DIR)/tests/source/test_format.cc.o $(OBJ_DIR)/tests/source/test_protocol.cc.o $(OBJ_DIR)/tests/source/test_lsp.cc.o $(OBJ_DIR)/tests/source/test_workspace_index.cc.o $(OBJ_DIR)/tests/source/test_transport.cc.o $(OBJ_DIR)/tests/source/test_compilation_database.cc.o,$(ALL_TEST_OBJS)): $(OBJ_DIR)/%.o: $(TOPLEVEL)/% $(COMPILER_PCMS)
	$(call compile-cxx,$<,$@,$(LOCAL_CXXFLAGS))

clean:
	rm -f $(ALL_TEST_OBJS) $(ALL_DEPS) $(ALL_TEST_BINS)
	rm -rf $(BUILD_DIR)/bin/tests

-include $(ALL_DEPS)

include $(STD_MK)
