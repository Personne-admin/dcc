include $(RULES_MK)

LOCAL_CXXFLAGS := -I$(TOPLEVEL)/compiler/source $(LLVM_DEFS) $(LLVM_CXXFLAGS)
DPM_LDFLAGS := $(LLVM_LDFLAGS)

.PHONY: all clean

BINARY := $(BUILD_DIR)/bin/dpm$(EXE_SUFFIX)
CORE_LIB := $(BUILD_DIR)/lib/libdpmcore.a
COMPILER_LIB := $(BUILD_DIR)/lib/libcompiler.a

SCHEMA_DST := $(BUILD_DIR)/include/dpm/v1.dc

all: $(BINARY) $(SCHEMA_DST)

$(SCHEMA_DST): $(TOPLEVEL)/dpm/schema/dpm/v1.dc
	$(call MSG,SCHEMA,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)cp $< $@

MODULE_SRCS := $(call find-sources,$(TOPLEVEL)/dpm/source,cppm)

ifneq ($(strip $(MODULE_SRCS)),)
  include $(MODULES_MK)
endif

ALL_CC_SRCS := $(call find-sources,$(TOPLEVEL)/dpm/source,cc)
OBJS := $(call objs-from-srcs,$(ALL_CC_SRCS))
DEPS := $(OBJS:.o=.d)

COMPILER_PCMS := $(wildcard $(PCM_DIR)/dcc.*.pcm)
DPM_PCM_FILES := $(filter $(PCM_DIR)/dpm.%.pcm,$(MODULE_PCM_FILES))

$(DPM_PCM_FILES): $(COMPILER_PCMS)

$(eval $(call track-archive-inputs,$(CORE_LIB),$(MODULE_OBJS)))

$(CORE_LIB): $(MODULE_OBJS)
	$(call create-archive,$@,$(MODULE_OBJS))

$(BINARY): $(OBJS) $(CORE_LIB) $(COMPILER_LIB)
	$(call link-binary,$@,$^,$(DPM_LDFLAGS))

$(OBJS): $(OBJ_DIR)/%.o: $(COMPILER_PCMS) $(MODULE_PCM_FILES)
	$(call compile-cxx,$(TOPLEVEL)/$*,$@,$(LOCAL_CXXFLAGS))

clean:
	rm -f $(OBJS) $(DEPS) $(MODULE_OBJS) $(MODULE_PCM_FILES) $(CORE_LIB) $(CORE_LIB).objects $(BINARY)
	rm -rf $(BUILD_DIR)/include/dpm
	rm -f $(DEP_DIR)/dpm.modules.*.d.mk

-include $(DEPS)

include $(STD_MK)

include $(COMPDB_MK)
