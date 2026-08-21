# Builds one adapter against one installed FPP source tree. FPP_SRC must
# point at the src directory of the FPP major being built for, which is
# how the packaging step compiles on the host: against that host's own
# installed headers, never against a version assumed at release time.
#
#   make -C native/adapters fpp9  FPP_SRC=/opt/fpp/src
#   make -C native/adapters fpp10 FPP_SRC=/opt/fpp/src
#
# The two are separate targets producing separate shared objects on
# purpose. FPP 10 versions and checks its plugin ABI at dlopen, so one
# binary cannot serve both majors, and mixing their surfaces in one
# translation unit would hide that behind an ifdef.

CXX      ?= c++
CXXSTD   ?= -std=c++20
CXXFLAGS ?= $(CXXSTD) -O2 -fPIC -Wall -Wextra -Werror
BUILD    ?= build
FPP_SRC  ?= /opt/fpp/src

CORE_DIR     := ..
CORE_SOURCES := $(wildcard $(CORE_DIR)/src/*.cpp)

JSONCPP_INCLUDE := $(shell pkg-config --cflags-only-I jsoncpp 2>/dev/null | sed 's/-I/-isystem /g')
ifeq ($(strip $(JSONCPP_INCLUDE)),)
JSONCPP_INCLUDE := -isystem /usr/include/jsoncpp
endif

# FPP's own headers are included with -isystem so their warnings do not
# drown this repository's. -Werror applies to this code either way.
INCLUDES := -I$(CORE_DIR)/include -Ishared -isystem $(FPP_SRC) $(JSONCPP_INCLUDE)

.PHONY: fpp9
fpp9: $(BUILD)/fpp9/libshowmesh-fpp9.so

.PHONY: fpp10
fpp10: $(BUILD)/fpp10/libshowmesh-fpp10.so

$(BUILD)/fpp9/libshowmesh-fpp9.so: fpp9/plugin.cpp $(CORE_SOURCES)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -shared $^ -o $@

$(BUILD)/fpp10/libshowmesh-fpp10.so: fpp10/plugin.cpp $(CORE_SOURCES)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -shared $^ -o $@

# The loader looks up createPlugin() by name, so a build that produced a
# library without it would install cleanly and then fail at load with
# nothing but a log line. FPP 10 additionally refuses at dlopen any plugin
# whose ABI-version and logger-layout fingerprints disagree with its own,
# and those come from weak definitions in FPP's own header: a plugin that
# does not export them is either built against the wrong headers or had
# them stripped. Checked here rather than discovered on a show host.
.PHONY: verify-fpp9
verify-fpp9:
	@$(call require_symbols,$(BUILD)/fpp9/libshowmesh-fpp9.so,createPlugin)

.PHONY: verify-fpp10
verify-fpp10:
	@$(call require_symbols,$(BUILD)/fpp10/libshowmesh-fpp10.so,createPlugin fpp_plugin_api_version fpp_plugin_supports_unload fpp_logger_instance_abi_size fpp_logger_abi_span)
	@echo "verify-fpp10: FPP header declares plugin ABI version $$(grep -E '^#define FPP_PLUGIN_API_VERSION' $(FPP_SRC)/Plugin.h | awk '{print $$3}')"

define require_symbols
	if [ ! -f "$(1)" ]; then echo "$(1) was not built"; exit 1; fi; \
	for sym in $(2); do \
		if nm -D --defined-only "$(1)" | grep -qE " $$sym$$"; then \
			echo "$(1) exports $$sym"; \
		else \
			echo "$(1) does NOT export $$sym"; exit 1; \
		fi; \
	done
endef

.PHONY: clean
clean:
	rm -rf $(BUILD)
