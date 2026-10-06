include $(CONFIG_MK)

define src-to-obj
$(OBJ_DIR)/$(patsubst $(TOPLEVEL)/%,%,$(abspath $(1))).o
endef

define find-sources
$(shell find $(1) -name '*.$(2)' 2>/dev/null)
endef

define objs-from-srcs
$(foreach s,$(1),$(call src-to-obj,$(s)))
endef

define cppm-to-pcm
$(PCM_DIR)/$(patsubst $(TOPLEVEL)/%,%,$(abspath $(1))).pcm
endef

define cppm-to-obj
$(OBJ_DIR)/$(patsubst $(TOPLEVEL)/%,%,$(abspath $(1))).o
endef

ifndef V
  Q := @
  MSG = @printf "  %-8s %s\n" "$(1)" "$(2)"
else
  Q :=
  MSG = @true
endif

define compile-cxx
	@mkdir -p $(dir $(2))
	$(call MSG,CXX,$(1))
	$(Q)$(CXX) $(BASE_CXXFLAGS) -MMD -MP -fprebuilt-module-path=$(PCM_DIR) $(3) -c $(1) -o $(2)
endef

define precompile-module
	@mkdir -p $(dir $(2))
	$(call MSG,PCM,$(1))
	$(Q)$(CXX) $(BASE_CXXFLAGS) -fprebuilt-module-path=$(PCM_DIR) $(3) --precompile $(1) -o $(2)
endef

define compile-pcm
	@mkdir -p $(dir $(2))
	$(call MSG,OBJ,$(1))
	$(Q)$(CXX) $(BASE_CXXFLAGS) -Wno-unused-command-line-argument -fprebuilt-module-path=$(PCM_DIR) -c $(1) -o $(2)
endef

define create-archive
	@mkdir -p $(dir $(1))
	$(call MSG,AR,$(1))
	$(Q)rm -f $(1).tmp
	$(Q)$(AR) rcs $(1).tmp $(2)
	$(Q)mv -f $(1).tmp $(1)
endef

define link-binary
	@mkdir -p $(dir $(1))
	$(call MSG,LD,$(1))
	$(Q)$(CXX) $(BASE_LDFLAGS) $(2) $(3) $(LINK_TAIL_LDFLAGS) -o $(1)
	$(DSYM_CMD)
endef

.PHONY: check-archive-inputs

define track-archive-inputs
$(1).objects: check-archive-inputs
	@mkdir -p $$(dir $$@)
	@printf '%s\n' '$(2)' > $$@.tmp
	@if cmp -s $$@.tmp $$@; then rm -f $$@.tmp; else mv -f $$@.tmp $$@; fi

$(1): $(1).objects
endef
