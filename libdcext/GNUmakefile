include $(RULES_MK)

TARGET ?= x86_64-elf
DCC ?= $(BUILD_DIR)/bin/dcc

ifeq ($(TARGET),x86_64-elf)
  PLATFORM_DIR := linux
else ifeq ($(TARGET),x86_64-coff)
  PLATFORM_DIR := nt
else
  $(error libdcext does not yet support target $(TARGET))
endif

DC_SRCS := $(call find-sources,$(TOPLEVEL)/libdcext/std,dc)
ASM_SRCS := $(call find-sources,$(TOPLEVEL)/libdcext/$(PLATFORM_DIR),asm)

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
	$(Q)$(DCC) -c -target $(TARGET) -I$(TOPLEVEL)/libdcext -o $@ $<

$(ASM_OBJS): $(OBJ_DIR)/%.asm.o: $(TOPLEVEL)/%.asm
	@mkdir -p $(dir $@)
	$(call MSG,ASM,$<)
	$(Q)$(CC) -c -target $(TARGET) -x assembler-with-cpp -o $@ $<

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
