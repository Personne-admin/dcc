ifeq ($(CROSS),windows)
  MINGW_SYSROOT ?= /opt/llvm-mingw
  MINGW_TARGET ?= x86_64-w64-mingw32
  CXX := $(MINGW_SYSROOT)/bin/$(MINGW_TARGET)-clang++
  CC := $(MINGW_SYSROOT)/bin/$(MINGW_TARGET)-clang
  AR := $(MINGW_SYSROOT)/bin/$(MINGW_TARGET)-llvm-ar
  EXE_SUFFIX := .exe
else
  CXX := clang++
  CC := clang
  AR := ar
  EXE_SUFFIX :=
endif

INSTALL ?= install

STATIC_LINK ?= 0

ifeq ($(strip $(DCC_VERSION)),)
  DCC_VERSION := 0.3.0
endif

SCAN_DEPS ?= $(shell which clang-scan-deps 2>/dev/null)
ifneq ($(filter clean distclean,$(MAKECMDGOALS)),)
else
  ifeq ($(SCAN_DEPS),)
    $(error clang-scan-deps not found)
  endif
endif

PYTHON ?= python3

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

CXXSTD := -std=c++26
WARNS := -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow \
            -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align \
            -Wformat=2 -Wimplicit-fallthrough

ifeq ($(BUILD_TYPE),release)
  OPT_FLAGS   := -O2 -DNDEBUG
  DEBUG_FLAGS :=
else ifeq ($(BUILD_TYPE),relwithdebinfo)
  OPT_FLAGS   := -O2 -DNDEBUG
  DEBUG_FLAGS := -g2
else
  OPT_FLAGS   := -O0
  DEBUG_FLAGS := -g3
endif

SAN_FLAGS :=
ifeq ($(BUILD_TYPE),debug)
  ifeq ($(ENABLE_ASAN),1)
    SAN_FLAGS := -fsanitize=undefined,address -fno-omit-frame-pointer
  endif
endif

include $(TOPLEVEL)/mk/llvm.mk

ifeq ($(UNAME_S),Darwin)
  DSYM_CMD = dsymutil $@ 2>/dev/null || true
else
  DSYM_CMD = @true
endif

ifeq ($(CROSS),windows)
  STD_MODULE_SRC ?= $(MINGW_SYSROOT)/share/libc++/v1/std.cppm
  STD_COMPAT_SRC ?= $(MINGW_SYSROOT)/share/libc++/v1/std.compat.cppm
  STD_INCLUDE_DIR := $(MINGW_SYSROOT)/generic-w64-mingw32/include/c++/v1
else
  STD_MODULE_SRC ?= $(shell find /usr -name 'std.cppm' -path '*/libc++/*' 2>/dev/null | head -1)
  STD_COMPAT_SRC ?= $(shell find /usr -name 'std.compat.cppm' -path '*/libc++/*' 2>/dev/null | head -1)
endif

ifneq ($(filter clean distclean,$(MAKECMDGOALS)),)
else
  ifeq ($(STD_MODULE_SRC),)
    $(error Cannot find std.cppm)
  endif
endif

STDLIB_FLAGS := -stdlib=libc++

DCC_INSTALL_PREFIX ?= $(abspath $(PREFIX))
PREFIX_STAMP := $(DEP_DIR)/dcc-prefix.stamp
DCC_PREFIX_DEF := -DDCC_INSTALL_PREFIX='"$(DCC_INSTALL_PREFIX)"'

BASE_CXXFLAGS := $(CXXSTD) $(WARNS) $(OPT_FLAGS) $(DEBUG_FLAGS) $(SAN_FLAGS) \
                 $(LLVM_CXXFLAGS) $(LLVM_DEFS) $(STDLIB_FLAGS) $(DCC_PREFIX_DEF) $(if $(filter windows,$(CROSS)),,--gcc-install-dir="")

ifeq ($(CROSS),windows)
  BASE_LDFLAGS := $(SAN_FLAGS) $(STDLIB_FLAGS) -static
  LINK_TAIL_LDFLAGS :=
  CMARK_LIBS := -lcmark
else ifeq ($(STATIC_LINK),1)
  BASE_LDFLAGS := $(SAN_FLAGS) -nostdlib++ -static-libgcc
  LINK_TAIL_LDFLAGS := -Wl,-Bstatic -lc++ -lc++abi -Wl,-Bdynamic
  CMARK_LIBS := -l:libcmark.a
else
  BASE_LDFLAGS := $(SAN_FLAGS) $(STDLIB_FLAGS) -lc++abi
  LINK_TAIL_LDFLAGS :=
  CMARK_LIBS := -lcmark
endif

.DEFAULT_GOAL := all

.PHONY: FORCE
FORCE:

$(PREFIX_STAMP): FORCE
	@mkdir -p $(dir $@)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(DCC_INSTALL_PREFIX)" ]; then echo "$(DCC_INSTALL_PREFIX)" > $@; fi

VERSION_STAMP := $(DEP_DIR)/dcc-version.stamp

$(VERSION_STAMP): FORCE
	@mkdir -p $(dir $@)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(DCC_VERSION)" ]; then echo "$(DCC_VERSION)" > $@; fi
