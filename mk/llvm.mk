LLVM_CONFIG ?= llvm-config

# Windows-targeted LLVM has no distro package to `llvm-config` against, and a cross-compiled
# llvm-config itself would be a Windows .exe this host can't run to ask it anything. Instead
# this points at a locally built, static, X86-only LLVM (core/native/orcjit/support and their
# transitive deps -- the same component set the native branch below asks llvm-config for),
# installed via CMake's `install-llvm-headers` + copying every resulting lib/*.a. See the
# LLVM Windows toolchain notes for how to (re)produce this prefix.
LLVM_MINGW_TARGET_PREFIX ?= $(HOME)/.local/share/llvm-mingw-target

ifeq ($(ENABLE_LLVM),1)
  ifeq ($(CROSS),windows)
    LLVM_MINGW_TARGET_LIBS := $(wildcard $(LLVM_MINGW_TARGET_PREFIX)/lib/*.a)
    ifeq ($(LLVM_MINGW_TARGET_LIBS),)
      $(error ENABLE_LLVM=1 with CROSS=windows needs a Windows-targeted LLVM at LLVM_MINGW_TARGET_PREFIX=$(LLVM_MINGW_TARGET_PREFIX) (no lib/*.a found); build one, or pass ENABLE_LLVM=0 for the em64t-only backend)
    endif
    LLVM_CXXFLAGS := -I$(LLVM_MINGW_TARGET_PREFIX)/include
    # static libLLVM* archives reference each other in dependency cycles (e.g. Core <-> Support
    # forward references), which plain -l ordering can't satisfy; --start-group/--end-group
    # lets the linker keep re-scanning the group until everything resolves. The handful of
    # Win32 system libs (psapi/shell32/ole32/uuid/advapi32/version) cover what LLVM's Support
    # library needs for process/registry/COM queries on Windows.
    LLVM_LDFLAGS := -L$(LLVM_MINGW_TARGET_PREFIX)/lib \
                     -Wl,--start-group $(patsubst lib%.a,-l%,$(notdir $(LLVM_MINGW_TARGET_LIBS))) -Wl,--end-group \
                     -lpsapi -lshell32 -lole32 -luuid -ladvapi32 -lversion -lntdll
  else
    LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cppflags 2>/dev/null)
    LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags --libs core native orcjit support --system-libs 2>/dev/null)
  endif
  LLVM_DEFS := -DDCC_ENABLE_LLVM=1
else
  LLVM_CXXFLAGS :=
  LLVM_LDFLAGS  :=
  LLVM_DEFS     :=
endif
