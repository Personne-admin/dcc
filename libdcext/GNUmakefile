include $(RULES_MK)

TARGET ?= x86_64-linux
BACKEND ?= llvm
LIBDCEXT_OPT ?= O2
DCC ?= $(NATIVE_BUILD_DIR)/bin/dcc
MINGW_SYSROOT ?= /opt/llvm-mingw
MINGW_TARGET ?= x86_64-w64-mingw32
MINGW_CC := $(wildcard $(MINGW_SYSROOT)/bin/$(MINGW_TARGET)-clang)

ifeq ($(TARGET),x86_64-elf)
  override TARGET := x86_64-linux
else ifeq ($(TARGET),x86_64-coff)
  override TARGET := x86_64-windows
endif

ifeq ($(TARGET),x86_64-linux)
  PLATFORM_DIR := linux
  DCC_TARGET := x86_64-elf
  TARGET_OS := linux
else ifeq ($(TARGET),x86_64-windows)
  PLATFORM_DIR := windows
  DCC_TARGET := x86_64-coff
  TARGET_OS := windows
else ifeq ($(TARGET),x86_64-freestanding)
  PLATFORM_DIR :=
  DCC_TARGET := x86_64-elf
  TARGET_OS := freestanding
else
  $(error libdcext does not yet support target $(TARGET); expected x86_64-linux, x86_64-windows, or x86_64-freestanding)
endif

ifeq ($(filter $(BACKEND),llvm em64t),)
  $(error libdcext does not yet support backend $(BACKEND); expected llvm or em64t)
endif

ifeq ($(TARGET_OS),windows)
  ASM_CC := $(if $(MINGW_CC),$(MINGW_CC),$(CC))
  ASM_TARGET := $(if $(MINGW_CC),$(MINGW_TARGET),$(DCC_TARGET))
else
  ASM_CC := $(CC)
  ASM_TARGET := $(DCC_TARGET)
endif

OBJ_DIR := $(OBJ_DIR)/libdcext-$(TARGET)-$(BACKEND)

FREESTANDING_EXCLUDE := $(TOPLEVEL)/libdcext/std/sys/linux/syscall.dc $(TOPLEVEL)/libdcext/std/sys/win/imports.dc

TOPLEVEL_SRCS := $(TOPLEVEL)/libdcext/assert.dc

ifeq ($(TARGET),x86_64-freestanding)
  DC_SRCS := $(filter-out $(FREESTANDING_EXCLUDE),$(call find-sources,$(TOPLEVEL)/libdcext/std,dc)) $(TOPLEVEL_SRCS)
  ASM_SRCS :=
else
  DC_SRCS := $(call find-sources,$(TOPLEVEL)/libdcext/std,dc) $(TOPLEVEL_SRCS)
  ASM_SRCS := $(call find-sources,$(TOPLEVEL)/libdcext/$(PLATFORM_DIR),asm)
endif

DC_OBJS := $(call objs-from-srcs,$(DC_SRCS))
ASM_OBJS := $(call objs-from-srcs,$(ASM_SRCS))
ALL_OBJS := $(DC_OBJS) $(ASM_OBJS)

LIBDCEXT_A := $(BUILD_DIR)/lib/libdcext-$(TARGET_OS)-$(BACKEND).a
HEADER_SRC := $(TOPLEVEL)/libdcext/std
STD_SRC := $(TOPLEVEL)/libdcext/std.dc
ASSERT_SRC := $(TOPLEVEL)/libdcext/assert.dc
HEADER_DST := $(BUILD_DIR)/include/std

.PHONY: all install uninstall clean headers

all: $(LIBDCEXT_A) headers

headers:
	@rm -rf $(HEADER_DST)
	@mkdir -p $(BUILD_DIR)/include
	$(Q)cp -r $(HEADER_SRC) $(BUILD_DIR)/include/
	$(Q)cp $(STD_SRC) $(BUILD_DIR)/include/
	$(Q)cp $(ASSERT_SRC) $(BUILD_DIR)/include/

TARGET_STAMP := $(BUILD_DIR)/lib/.libdcext-target-stamp

.PHONY: FORCE
FORCE:

$(TARGET_STAMP): FORCE
	@mkdir -p $(dir $@)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(TARGET)-$(BACKEND)" ]; then echo "$(TARGET)-$(BACKEND)" > $@; fi

$(LIBDCEXT_A): $(ALL_OBJS) $(DCC) $(TARGET_STAMP)
	@rm -f $@
	$(call create-archive,$@,$(ALL_OBJS))

$(DC_OBJS): $(DC_SRCS)

$(DC_OBJS): $(OBJ_DIR)/%.dc.o: $(TOPLEVEL)/%.dc $(DCC)
	@mkdir -p $(dir $@)
	$(call MSG,DCC,$<)
	$(Q)$(DCC) -c -flibdcext $(TARGET_OS) -target $(DCC_TARGET) -fbackend $(BACKEND) -$(LIBDCEXT_OPT) -I$(TOPLEVEL)/libdcext -o $@ $<

$(ASM_OBJS): $(OBJ_DIR)/%.asm.o: $(TOPLEVEL)/%.asm
	@mkdir -p $(dir $@)
	$(call MSG,ASM,$<)
	$(Q)$(ASM_CC) -c -target $(ASM_TARGET) -x assembler-with-cpp -o $@ $<

COMPDB_TARGETS ?= x86_64-linux x86_64-windows
COMPDB_SCRIPT := $(TOPLEVEL)/mk/gen_compdb.py
COMPDB_FRAGMENTS_DIR := $(BUILD_DIR)/compdb

.PHONY: compdb-fragment compdb-fragment-one

compdb-fragment:
	@mkdir -p $(COMPDB_FRAGMENTS_DIR)
	@for t in $(COMPDB_TARGETS); do \
		$(MAKE) --no-print-directory compdb-fragment-one TARGET=$$t; \
	done

compdb-fragment-one:
	$(Q)$(PYTHON) $(COMPDB_SCRIPT) fragment --mode plain \
		--cxx "$(DCC)" \
		--cxxflags "-c -flibdcext $(TARGET_OS) -target $(DCC_TARGET) -$(LIBDCEXT_OPT) -I$(TOPLEVEL)/libdcext" \
		--toplevel "$(TOPLEVEL)" \
		--output "$(COMPDB_FRAGMENTS_DIR)/libdcext-$(TARGET)-dc.json" \
		$(DC_SRCS)
	$(Q)$(PYTHON) $(COMPDB_SCRIPT) fragment --mode plain \
		--cxx "$(ASM_CC)" \
		--cxxflags "-c -target $(ASM_TARGET) -x assembler-with-cpp" \
		--toplevel "$(TOPLEVEL)" \
		--output "$(COMPDB_FRAGMENTS_DIR)/libdcext-$(TARGET)-asm.json" \
		$(ASM_SRCS)

install: all
	$(call MSG,INSTALL,$(DESTDIR)$(LIBDIR)/$(notdir $(LIBDCEXT_A)))
	$(Q)$(INSTALL) -d $(DESTDIR)$(LIBDIR)
	$(Q)$(INSTALL) -m 644 $(LIBDCEXT_A) $(DESTDIR)$(LIBDIR)/$(notdir $(LIBDCEXT_A))
	$(call MSG,INSTALL,$(DESTDIR)$(INCLUDEDIR)/std/)
	$(Q)$(INSTALL) -d $(DESTDIR)$(INCLUDEDIR)/std
	$(Q)cp -r $(HEADER_DST)/* $(DESTDIR)$(INCLUDEDIR)/std/

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(notdir $(LIBDCEXT_A))
	find $(DESTDIR)$(INCLUDEDIR)/std -name '*.dc' -delete 2>/dev/null || true

clean:
	rm -f $(DC_OBJS) $(ASM_OBJS) $(LIBDCEXT_A) $(TARGET_STAMP)
	rm -rf $(HEADER_DST)
