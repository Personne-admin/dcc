include $(RULES_MK)

ifeq ($(strip $(DCC_VERSION)),)
DCC_VERSION := 0.3.0
endif
DCC_GIT_HASH ?= $(shell git -C $(TOPLEVEL) rev-parse --short HEAD 2>/dev/null || echo unknown)
LOCAL_CXXFLAGS := -I$(TOPLEVEL)/compiler/source $(LLVM_DEFS) $(LLVM_CXXFLAGS) -DDCC_VERSION='"$(DCC_VERSION)"' -DDCC_GIT_HASH='"$(DCC_GIT_HASH)"'
DRIVER_LDFLAGS := $(LLVM_LDFLAGS)

.PHONY: all install clean

BINARY := $(BUILD_DIR)/bin/dcc$(EXE_SUFFIX)
COMPILER_LIB := $(BUILD_DIR)/lib/libcompiler.a

all: $(BINARY)

MODULE_SRCS ?=

ALL_CC_SRCS := $(call find-sources,$(TOPLEVEL)/driver/source,cc)
OBJS := $(call objs-from-srcs,$(ALL_CC_SRCS))
DEPS := $(OBJS:.o=.d)

COMPILER_PCMS := $(wildcard $(PCM_DIR)/dcc.*.pcm)

$(BINARY): $(OBJS) $(COMPILER_LIB)
	$(call link-binary,$@,$^,$(DRIVER_LDFLAGS))

$(OBJS): $(OBJ_DIR)/%.o: $(COMPILER_PCMS)
	$(call compile-cxx,$(TOPLEVEL)/$*,$@,$(LOCAL_CXXFLAGS))

install: $(BINARY)
	$(call MSG,INSTALL,$(DESTDIR)$(BINDIR)/dcc)
	$(Q)$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(Q)$(INSTALL) -m 755 $(BINARY) $(DESTDIR)$(BINDIR)/dcc

clean:
	rm -f $(OBJS) $(DEPS) $(BINARY)

-include $(DEPS)

include $(STD_MK)

include $(COMPDB_MK)
