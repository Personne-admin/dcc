CXX := clang++
CC := clang
AR := ar
INSTALL ?= install

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

STD_MODULE_SRC := $(shell find /usr -name 'std.cppm' -path '*/libc++/*' 2>/dev/null | head -1)
STD_COMPAT_SRC := $(shell find /usr -name 'std.compat.cppm' -path '*/libc++/*' 2>/dev/null | head -1)

ifneq ($(filter clean distclean,$(MAKECMDGOALS)),)
else
  ifeq ($(STD_MODULE_SRC),)
    $(error Cannot find std.cppm)
  endif
endif

STDLIB_FLAGS := -stdlib=libc++

BASE_CXXFLAGS := $(CXXSTD) $(WARNS) $(OPT_FLAGS) $(DEBUG_FLAGS) $(SAN_FLAGS) \
                 $(LLVM_CXXFLAGS) $(LLVM_DEFS) $(STDLIB_FLAGS) --gcc-install-dir=""

BASE_LDFLAGS := $(SAN_FLAGS) $(STDLIB_FLAGS) -lc++abi
