LLVM_CONFIG ?= llvm-config

LLVM_MINGW_TARGET_PREFIX ?= $(HOME)/.local/share/llvm-mingw-target

ifeq ($(ENABLE_LLVM),1)
  ifeq ($(CROSS),windows)
    LLVM_MINGW_TARGET_LIBS := $(wildcard $(LLVM_MINGW_TARGET_PREFIX)/lib/*.a)
    ifeq ($(LLVM_MINGW_TARGET_LIBS),)
      $(error ENABLE_LLVM=1 with CROSS=windows needs a Windows-targeted LLVM at LLVM_MINGW_TARGET_PREFIX=$(LLVM_MINGW_TARGET_PREFIX) (no lib/*.a found); build one, or pass ENABLE_LLVM=0 for the em64t-only backend)
    endif
    LLVM_CXXFLAGS := -I$(LLVM_MINGW_TARGET_PREFIX)/include
    LLVM_LDFLAGS := -L$(LLVM_MINGW_TARGET_PREFIX)/lib \
                     -Wl,--start-group $(patsubst lib%.a,-l%,$(notdir $(LLVM_MINGW_TARGET_LIBS))) -Wl,--end-group \
                     -lpsapi -lshell32 -lole32 -luuid -ladvapi32 -lversion -lntdll
  else
    LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cppflags 2>/dev/null)
    LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) $(if $(filter 1,$(STATIC_LINK)),--link-static) --ldflags --libs core native orcjit support --system-libs 2>/dev/null)
  endif
  LLVM_DEFS := -DDCC_ENABLE_LLVM=1
else
  LLVM_CXXFLAGS :=
  LLVM_LDFLAGS  :=
  LLVM_DEFS     :=
endif
