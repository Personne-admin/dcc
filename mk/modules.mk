_MODULES_DEP_MK := $(DEP_DIR)/$(notdir $(CURDIR)).modules.$(ENABLE_LLVM).d.mk
_SCAN_CXXFLAGS := $(BASE_CXXFLAGS) $(LOCAL_CXXFLAGS)
_SCANNED_MODULE_SRCS := $(shell sed -n 's/^SCANNED_MODULE_SRCS := //p' "$(_MODULES_DEP_MK)" 2>/dev/null)

_NEEDS_SCAN := $(shell \
    if [ ! -f "$(_MODULES_DEP_MK)" ] || [ "$(sort $(MODULE_SRCS))" != "$(_SCANNED_MODULE_SRCS)" ]; then \
    echo yes; \
else \
    for f in $(MODULE_SRCS); do \
    if [ "$$f" -nt "$(_MODULES_DEP_MK)" ]; then \
    echo yes; \
    break; \
    fi; \
    done; \
    fi)

ifeq ($(_NEEDS_SCAN),yes)
  $(info   SCAN     $(words $(MODULE_SRCS)) module sources...)
  _SCAN_RESULT := $(shell \
      $(PYTHON) $(SCAN_SCRIPT) \
      --cxx "$(CXX)" \
      --cxxflags "$(_SCAN_CXXFLAGS)" \
      --scan-deps "$(SCAN_DEPS)" \
      --pcm-dir "$(PCM_DIR)" \
      --obj-dir "$(OBJ_DIR)" \
      --dep-dir "$(DEP_DIR)" \
      --toplevel "$(TOPLEVEL)" \
      --output "$(_MODULES_DEP_MK)" \
      $(MODULE_SRCS) 2>&1; echo "EXIT:$$?")

  ifneq ($(findstring EXIT:0,$(_SCAN_RESULT)),EXIT:0)
    $(error Module scan failed: $(_SCAN_RESULT))
  endif
endif

include $(_MODULES_DEP_MK)
