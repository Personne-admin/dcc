include $(RULES_MK)

TARGET ?= x86_64-linux
DCC ?= $(BUILD_DIR)/bin/dcc

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

OBJ_DIR := $(OBJ_DIR)/libdcext-$(TARGET)

FREESTANDING_EXCLUDE := $(TOPLEVEL)/libdcext/std/sys/linux/syscall.dc $(TOPLEVEL)/libdcext/std/sys/win/imports.dc

ifeq ($(TARGET),x86_64-freestanding)
  DC_SRCS := $(filter-out $(FREESTANDING_EXCLUDE),$(call find-sources,$(TOPLEVEL)/libdcext/std,dc))
  ASM_SRCS :=
else
  DC_SRCS := $(call find-sources,$(TOPLEVEL)/libdcext/std,dc)
  ASM_SRCS := $(call find-sources,$(TOPLEVEL)/libdcext/$(PLATFORM_DIR),asm)
endif

DC_OBJS := $(call objs-from-srcs,$(DC_SRCS))
ASM_OBJS := $(call objs-from-srcs,$(ASM_SRCS))
ALL_OBJS := $(DC_OBJS) $(ASM_OBJS)

LIBDCEXT_A := $(BUILD_DIR)/lib/libdcext.a
HEADER_SRC := $(TOPLEVEL)/libdcext/std
STD_SRC := $(TOPLEVEL)/libdcext/std.dc
HEADER_DST := $(BUILD_DIR)/include/std

.PHONY: all install uninstall clean headers

all: $(LIBDCEXT_A) headers

headers:
	@rm -rf $(HEADER_DST)
	@mkdir -p $(BUILD_DIR)/include
	$(Q)cp -r $(HEADER_SRC) $(BUILD_DIR)/include/
	$(Q)cp $(STD_SRC) $(BUILD_DIR)/include/

$(LIBDCEXT_A): $(ALL_OBJS) $(DCC)
	$(call create-archive,$@,$(ALL_OBJS))

$(DC_OBJS): $(OBJ_DIR)/%.dc.o: $(TOPLEVEL)/%.dc $(DCC)
	@mkdir -p $(dir $@)
	$(call MSG,DCC,$<)
	$(Q)$(DCC) -c -flibdcext $(TARGET_OS) -target $(DCC_TARGET) -I$(TOPLEVEL)/libdcext -o $@ $<

$(ASM_OBJS): $(OBJ_DIR)/%.asm.o: $(TOPLEVEL)/%.asm
	@mkdir -p $(dir $@)
	$(call MSG,ASM,$<)
	$(Q)$(CC) -c -target $(DCC_TARGET) -x assembler-with-cpp -o $@ $<

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
		--cxxflags "-c -flibdcext $(TARGET_OS) -target $(DCC_TARGET) -I$(TOPLEVEL)/libdcext" \
		--toplevel "$(TOPLEVEL)" \
		--output "$(COMPDB_FRAGMENTS_DIR)/libdcext-$(TARGET)-dc.json" \
		$(DC_SRCS)
	$(Q)$(PYTHON) $(COMPDB_SCRIPT) fragment --mode plain \
		--cxx "$(CC)" \
		--cxxflags "-c -target $(DCC_TARGET) -x assembler-with-cpp" \
		--toplevel "$(TOPLEVEL)" \
		--output "$(COMPDB_FRAGMENTS_DIR)/libdcext-$(TARGET)-asm.json" \
		$(ASM_SRCS)

install: all
	$(call MSG,INSTALL,$(DESTDIR)$(LIBDIR)/libdcext.a)
	$(Q)$(INSTALL) -d $(DESTDIR)$(LIBDIR)
	$(Q)$(INSTALL) -m 644 $(LIBDCEXT_A) $(DESTDIR)$(LIBDIR)/libdcext.a
	$(call MSG,INSTALL,$(DESTDIR)$(INCLUDEDIR)/std/)
	$(Q)$(INSTALL) -d $(DESTDIR)$(INCLUDEDIR)/std
	$(Q)cp -r $(HEADER_DST)/* $(DESTDIR)$(INCLUDEDIR)/std/

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libdcext.a
	find $(DESTDIR)$(INCLUDEDIR)/std -name '*.dc' -delete 2>/dev/null || true

clean:
	rm -f $(DC_OBJS) $(ASM_OBJS) $(LIBDCEXT_A)
	rm -rf $(HEADER_DST)
