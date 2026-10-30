include $(RULES_MK)

DCCD_DCON ?= $(if $(filter windows,$(CROSS)),0,1)

ifeq ($(DCCD_DCON),0)
  BASE_CXXFLAGS += -DDCCD_NO_DCON
endif

LOCAL_CXXFLAGS := -I$(TOPLEVEL)/compiler/source $(LLVM_DEFS) $(LLVM_CXXFLAGS) -DDCC_VERSION='"$(DCC_VERSION)"'
DCCD_LDFLAGS := $(LLVM_LDFLAGS)

.PHONY: all clean

BINARY := $(BUILD_DIR)/bin/dccd$(EXE_SUFFIX)
COMPILER_LIB := $(BUILD_DIR)/lib/libcompiler.a
DPM_CORE_LIB := $(if $(filter 0,$(DCCD_DCON)),,$(BUILD_DIR)/lib/libdpmcore.a)

all: $(BINARY)

MODULE_SRCS := $(call find-sources,$(TOPLEVEL)/dccd/source,cppm)

ifneq ($(strip $(MODULE_SRCS)),)
  include $(MODULES_MK)
endif

ALL_CC_SRCS := $(call find-sources,$(TOPLEVEL)/dccd/source,cc)
OBJS := $(call objs-from-srcs,$(ALL_CC_SRCS))
DEPS := $(OBJS:.o=.d)

COMPILER_PCMS := $(wildcard $(PCM_DIR)/dcc.*.pcm) $(if $(filter 0,$(DCCD_DCON)),,$(wildcard $(PCM_DIR)/dpm.*.pcm))
DCCD_PCM_FILES := $(filter $(PCM_DIR)/dccd.%.pcm,$(MODULE_PCM_FILES))

$(DCCD_PCM_FILES): $(COMPILER_PCMS)

$(BINARY): $(MODULE_OBJS) $(OBJS) $(DPM_CORE_LIB) $(COMPILER_LIB)
	$(call link-binary,$@,$^,$(DCCD_LDFLAGS))

$(OBJS): $(OBJ_DIR)/%.o: $(COMPILER_PCMS) $(MODULE_PCM_FILES)
	$(call compile-cxx,$(TOPLEVEL)/$*,$@,$(LOCAL_CXXFLAGS))

clean:
	rm -f $(OBJS) $(DEPS) $(MODULE_OBJS) $(MODULE_PCM_FILES) $(BINARY)
	rm -f $(DEP_DIR)/dccd.modules.*.d.mk

-include $(DEPS)

include $(STD_MK)

include $(COMPDB_MK)
