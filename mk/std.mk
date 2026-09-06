STD_PCM := $(PCM_DIR)/std.pcm
STD_COMPAT_PCM := $(PCM_DIR)/std.compat.pcm
STD_INCLUDE_DIR ?= /usr/include/c++/v1

$(STD_PCM): $(STD_MODULE_SRC)
	@mkdir -p $(dir $@)
	$(call MSG,PCM,std)
	$(Q)$(CXX) $(BASE_CXXFLAGS) -Wno-reserved-module-identifier -I$(STD_INCLUDE_DIR) --precompile $< -o $@

$(STD_COMPAT_PCM): $(STD_COMPAT_SRC) $(STD_PCM)
	@mkdir -p $(dir $@)
	$(call MSG,PCM,std.compat)
	$(Q)$(CXX) $(BASE_CXXFLAGS) -Wno-reserved-module-identifier -fmodule-file=std=$(STD_PCM) --precompile $< -o $@

STD_OBJ := $(OBJ_DIR)/std.pcm.o
STD_COMPAT_OBJ := $(OBJ_DIR)/std.compat.pcm.o

$(STD_OBJ): $(STD_PCM)
	@mkdir -p $(dir $@)
	$(call MSG,OBJ,std.pcm)
	$(Q)$(CXX) $(BASE_CXXFLAGS) -c $< -o $@

$(STD_COMPAT_OBJ): $(STD_COMPAT_PCM)
	@mkdir -p $(dir $@)
	$(call MSG,OBJ,std.compat.pcm)
	$(Q)$(CXX) $(BASE_CXXFLAGS) -c $< -o $@

.PHONY: std-modules
std-modules: $(STD_PCM) $(STD_COMPAT_PCM) $(STD_OBJ) $(STD_COMPAT_OBJ)
