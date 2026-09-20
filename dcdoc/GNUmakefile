include $(RULES_MK)

LOCAL_CXXFLAGS := -I$(TOPLEVEL)/compiler/source

.PHONY: all clean

BINARY := $(BUILD_DIR)/bin/dcdoc$(EXE_SUFFIX)
COMPILER_LIB := $(BUILD_DIR)/lib/libcompiler.a

all: $(BINARY)

MODULE_SRCS := $(call find-sources,$(TOPLEVEL)/dcdoc/source,cppm)

ifneq ($(strip $(MODULE_SRCS)),)
  include $(MODULES_MK)
endif

ALL_CC_SRCS := $(call find-sources,$(TOPLEVEL)/dcdoc/source,cc)
OBJS := $(call objs-from-srcs,$(ALL_CC_SRCS))
DEPS := $(OBJS:.o=.d)

COMPILER_PCMS := $(wildcard $(PCM_DIR)/dcc.*.pcm)
DCDOC_PCM_FILES := $(filter $(PCM_DIR)/dcdoc.%.pcm,$(MODULE_PCM_FILES))

$(DCDOC_PCM_FILES): $(COMPILER_PCMS)

$(BINARY): $(MODULE_OBJS) $(OBJS) $(COMPILER_LIB)
	$(call link-binary,$@,$^,)

$(OBJS): $(OBJ_DIR)/%.o: $(COMPILER_PCMS) $(MODULE_PCM_FILES)
	$(call compile-cxx,$(TOPLEVEL)/$*,$@,$(LOCAL_CXXFLAGS))

clean:
	rm -f $(OBJS) $(DEPS) $(MODULE_OBJS) $(MODULE_PCM_FILES) $(BINARY)
	rm -f $(DEP_DIR)/dcdoc.modules.*.d.mk

-include $(DEPS)

include $(STD_MK)

include $(COMPDB_MK)
