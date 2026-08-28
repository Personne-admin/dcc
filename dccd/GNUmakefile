include $(RULES_MK)

LOCAL_CXXFLAGS := -I$(TOPLEVEL)/compiler/source $(LLVM_DEFS) $(LLVM_CXXFLAGS)
DCCD_LDFLAGS := $(LLVM_LDFLAGS)

.PHONY: all clean

BINARY := $(BUILD_DIR)/bin/dccd
COMPILER_LIB := $(BUILD_DIR)/lib/libcompiler.a

all: $(BINARY)

MODULE_SRCS := $(call find-sources,$(TOPLEVEL)/dccd/source,cppm)

ifneq ($(strip $(MODULE_SRCS)),)
  include $(MODULES_MK)
endif

ALL_CC_SRCS := $(call find-sources,$(TOPLEVEL)/dccd/source,cc)
OBJS := $(call objs-from-srcs,$(ALL_CC_SRCS))
DEPS := $(OBJS:.o=.d)

COMPILER_PCMS := $(wildcard $(PCM_DIR)/dcc.*.pcm)
DCCD_PCM_FILES := $(filter $(PCM_DIR)/dccd.%.pcm,$(MODULE_PCM_FILES))

$(DCCD_PCM_FILES): $(COMPILER_PCMS)

$(BINARY): $(MODULE_OBJS) $(OBJS) $(COMPILER_LIB)
	$(call link-binary,$@,$^,$(DCCD_LDFLAGS))

$(OBJS): $(OBJ_DIR)/%.o: $(COMPILER_PCMS) $(MODULE_PCM_FILES)
	$(call compile-cxx,$(TOPLEVEL)/$*,$@,$(LOCAL_CXXFLAGS))

clean:
	rm -f $(OBJS) $(DEPS) $(MODULE_OBJS) $(MODULE_PCM_FILES) $(BINARY)
	rm -f $(DEP_DIR)/dccd.modules.*.d.mk

-include $(DEPS)

include $(STD_MK)

include $(COMPDB_MK)
