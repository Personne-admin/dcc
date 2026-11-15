include $(RULES_MK)

FTXUI_ZIP := $(TOPLEVEL)/learndc/third_party/ftxui/ftxui-amalgamated-7.0.3.zip
FTXUI_SHA256 := 63d13bb65747bdbf0edf04d83ce490708022371cf378f15fad8489249ea4fd9a
LEARNDC_GEN := $(BUILD_DIR)/learndc
FTXUI_DIR := $(LEARNDC_GEN)/ftxui
FTXUI_HPP := $(FTXUI_DIR)/ftxui.hpp
FTXUI_CPP := $(FTXUI_DIR)/ftxui.cpp
FTXUI_OBJ := $(OBJ_DIR)/learndc/ftxui.o
CONTENT_ROOT := $(TOPLEVEL)/learndc/content
CONTENT_BLOB := $(LEARNDC_GEN)/learndc-content.bin
CONTENT_FILES := $(shell find $(CONTENT_ROOT) -type f 2>/dev/null)

ifeq ($(CROSS),windows)
  TUI_DEFS := -DUNICODE -D_UNICODE -DNOMINMAX
  LEARNDC_SYSLIBS :=
else
  TUI_DEFS :=
  LEARNDC_SYSLIBS :=
endif

LOCAL_CXXFLAGS := $(TUI_DEFS)

BINARY := $(BUILD_DIR)/bin/learndc$(EXE_SUFFIX)
CORE_LIB := $(BUILD_DIR)/lib/liblearndc.a
HOST_PROCESS_OBJ := $(OBJ_DIR)/dcc.host.process.pcm.o

.PHONY: all clean ftxui content

all: $(BINARY)

MODULE_SRCS := $(call find-sources,$(TOPLEVEL)/learndc/source,cppm)
ifeq ($(LEARNDC_STANDALONE),1)
  MODULE_SRCS += $(TOPLEVEL)/compiler/source/dcc.host.process.cppm
endif

ifneq ($(strip $(MODULE_SRCS)),)
  include $(MODULES_MK)
endif

LEARNDC_MODULE_OBJS := $(filter-out $(HOST_PROCESS_OBJ),$(MODULE_OBJS))
LEARNDC_PCM_FILES := $(filter $(PCM_DIR)/learndc.%.pcm,$(MODULE_PCM_FILES))

$(PCM_DIR)/learndc.cli.pcm: $(VERSION_STAMP) $(REVISION_STAMP)
$(OBJ_DIR)/learndc.cli.pcm.o: $(VERSION_STAMP) $(REVISION_STAMP)

ifneq ($(LEARNDC_STANDALONE),1)
$(LEARNDC_PCM_FILES): $(wildcard $(PCM_DIR)/dcc.host.process.pcm)
endif

$(FTXUI_HPP) $(FTXUI_CPP) &: $(FTXUI_ZIP) $(TOPLEVEL)/learndc/tools/extract_ftxui.py
	$(call MSG,EXTRACT,$(FTXUI_ZIP))
	$(Q)$(PYTHON) -I $(TOPLEVEL)/learndc/tools/extract_ftxui.py --zip $(FTXUI_ZIP) --sha256 $(FTXUI_SHA256) --out $(FTXUI_DIR)
	$(Q)touch $(FTXUI_HPP) $(FTXUI_CPP)

ftxui: $(FTXUI_HPP)

$(FTXUI_OBJ): $(FTXUI_CPP) $(FTXUI_HPP)
	@mkdir -p $(dir $@)
	$(call MSG,CXX,ftxui.cpp)
	$(Q)$(CXX) $(BASE_CXXFLAGS) $(TUI_DEFS) -w -O2 -g0 -I$(FTXUI_DIR) -c $(FTXUI_CPP) -o $@

$(CONTENT_BLOB): $(CONTENT_FILES) $(TOPLEVEL)/learndc/tools/bundle_content.py
	$(call MSG,BUNDLE,$(CONTENT_ROOT))
	$(Q)$(PYTHON) -I $(TOPLEVEL)/learndc/tools/bundle_content.py --root $(CONTENT_ROOT) --output $@
	$(Q)touch $@

content: $(CONTENT_BLOB)

CORE_CC_SRCS := $(TOPLEVEL)/learndc/source/bundle_data.cc
TUI_CC_SRCS := $(filter-out $(CORE_CC_SRCS) $(TOPLEVEL)/learndc/source/main.cc,$(call find-sources,$(TOPLEVEL)/learndc/source,cc))
MAIN_SRC := $(TOPLEVEL)/learndc/source/main.cc

CORE_CC_OBJS := $(call objs-from-srcs,$(CORE_CC_SRCS))
TUI_OBJS := $(call objs-from-srcs,$(TUI_CC_SRCS))
MAIN_OBJ := $(call objs-from-srcs,$(MAIN_SRC))
CC_OBJS := $(CORE_CC_OBJS) $(TUI_OBJS) $(MAIN_OBJ)
ALL_CC_SRCS := $(CORE_CC_SRCS) $(TUI_CC_SRCS) $(MAIN_SRC)
DEPS := $(CC_OBJS:.o=.d)

TUI_LIB := $(BUILD_DIR)/lib/liblearndc-tui.a

$(CORE_CC_OBJS): $(CONTENT_BLOB)
$(CORE_CC_OBJS): LOCAL_CXXFLAGS += --embed-dir=$(LEARNDC_GEN) -Wno-c23-extensions

$(TUI_OBJS) $(MAIN_OBJ): $(FTXUI_HPP)
$(TUI_OBJS) $(MAIN_OBJ): LOCAL_CXXFLAGS += -isystem $(FTXUI_DIR)

$(CC_OBJS): $(OBJ_DIR)/%.o: $(MODULE_PCM_FILES)
	$(call compile-cxx,$(TOPLEVEL)/$*,$@,$(LOCAL_CXXFLAGS))

$(eval $(call track-archive-inputs,$(CORE_LIB),$(LEARNDC_MODULE_OBJS) $(CORE_CC_OBJS)))

$(CORE_LIB): $(LEARNDC_MODULE_OBJS) $(CORE_CC_OBJS)
	$(call create-archive,$@,$(LEARNDC_MODULE_OBJS) $(CORE_CC_OBJS))

$(eval $(call track-archive-inputs,$(TUI_LIB),$(TUI_OBJS) $(FTXUI_OBJ)))

$(TUI_LIB): $(TUI_OBJS) $(FTXUI_OBJ)
	$(call create-archive,$@,$(TUI_OBJS) $(FTXUI_OBJ))

ifeq ($(CROSS),windows)
WINDOWS_RESOURCE := $(OBJ_DIR)/learndc/windows.o
$(WINDOWS_RESOURCE): $(TOPLEVEL)/mk/windows.rc $(TOPLEVEL)/mk/windows.manifest
	$(Q)mkdir -p $(dir $@)
	$(Q)$(MINGW_SYSROOT)/bin/$(MINGW_TARGET)-windres -I$(TOPLEVEL)/mk -i $< -o $@ -O coff
$(BINARY): $(WINDOWS_RESOURCE)
endif

$(BINARY): $(MAIN_OBJ) $(TUI_LIB) $(CORE_LIB) $(HOST_PROCESS_OBJ)
	$(call link-binary,$@,$^,$(LEARNDC_SYSLIBS))

clean:
	rm -f $(CC_OBJS) $(DEPS) $(LEARNDC_MODULE_OBJS) $(filter-out %/dcc.host.process.pcm,$(MODULE_PCM_FILES)) $(CORE_LIB) $(CORE_LIB).objects $(TUI_LIB) $(TUI_LIB).objects $(BINARY) $(FTXUI_OBJ)
	rm -rf $(LEARNDC_GEN)
	rm -f $(DEP_DIR)/learndc.modules.*.d.mk

-include $(DEPS)

include $(STD_MK)

include $(COMPDB_MK)
