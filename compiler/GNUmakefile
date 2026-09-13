include $(RULES_MK)

LOCAL_CXXFLAGS := -I$(TOPLEVEL)/compiler/source

STD_MODULES_BUILT := $(STD_PCM) $(STD_OBJ)

ALL_MODULE_SRCS := $(call find-sources,$(TOPLEVEL)/compiler/source,cppm)

MODULE_SRCS := $(filter-out %/dcc.backend.llvm.cppm,$(ALL_MODULE_SRCS))
ifeq ($(ENABLE_LLVM),1)
  MODULE_SRCS += $(filter %/dcc.backend.llvm.cppm,$(ALL_MODULE_SRCS))
endif

LOCAL_CXXFLAGS += $(LLVM_DEFS) $(LLVM_CXXFLAGS)

LIB := $(BUILD_DIR)/lib/libcompiler.a
all: $(LIB)

ifneq ($(strip $(MODULE_SRCS)),)
  include $(MODULES_MK)
endif

$(PCM_DIR)/dcc.config.pcm: $(PREFIX_STAMP)
$(OBJ_DIR)/dcc.config.pcm.o: $(PREFIX_STAMP)

CC_SRCS := $(call find-sources,$(TOPLEVEL)/compiler/source,cc)
CC_OBJS := $(call objs-from-srcs,$(CC_SRCS))

CC_DEPS := $(CC_OBJS:.o=.d)
ALL_OBJS := $(MODULE_OBJS) $(CC_OBJS)

.PHONY: all clean

$(LIB): $(ALL_OBJS)
	$(call create-archive,$@,$^)

$(CC_OBJS): $(OBJ_DIR)/%.o: $(MODULE_PCM_FILES)
	$(call compile-cxx,$(TOPLEVEL)/$*,$@,$(LOCAL_CXXFLAGS))

clean:
	rm -f $(ALL_OBJS) $(CC_DEPS) $(MODULE_PCM_FILES) $(LIB)
	rm -f $(STD_PCM) $(STD_COMPAT_PCM) $(STD_OBJ) $(STD_COMPAT_OBJ)
	rm -f $(DEP_DIR)/compiler.modules.*.d.mk

-include $(CC_DEPS)

include $(STD_MK)

include $(COMPDB_MK)
