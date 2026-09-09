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

JSONCPP_LIBS := $(shell pkg-config --libs jsoncpp 2>/dev/null)
ifeq ($(strip $(JSONCPP_LIBS)),)
JSONCPP_LIBS := -ljsoncpp
endif

# Ed25519 verification for the ADR-048 fallback program
# (fallback_program_verifier.h). libcrypto's runtime shared object is
# already on an FPP host regardless of this package, pulled in
# transitively by libcurl4-openssl-dev's own dependency on libcurl4;
# named here so the adapter links it explicitly rather than relying on
# whatever happens to already be loaded. That is a statement about the
# runtime .so on a deployed host, not about what this BUILD needs: the
# pkg-config calls below still require the libcrypto dev headers
# (openssl/evp.h) and .pc file to be installed on whatever machine runs
# this Makefile, the identical distinction ci.yml's own libssl-dev
# comment draws for libcurl4-openssl-dev's Suggests-vs-Depends split.
# Defined before INCLUDES below (a := assignment expands immediately, at
# the point it is written, not lazily): both majors' plugin.cpp reach
# fallback_activation_delivery.h now, so this is no longer only the test
# target's dependency.
OPENSSL_INCLUDE := $(shell pkg-config --cflags-only-I libcrypto 2>/dev/null | sed 's/-I/-isystem /g')
OPENSSL_LIBS := $(shell pkg-config --libs libcrypto 2>/dev/null)
ifeq ($(strip $(OPENSSL_LIBS)),)
OPENSSL_LIBS := -lcrypto
endif

# FPP's own headers are included with -isystem so their warnings do not
# drown this repository's. -Werror applies to this code either way.
INCLUDES := -I$(CORE_DIR)/include -Ishared -isystem $(FPP_SRC) $(JSONCPP_INCLUDE) $(OPENSSL_INCLUDE)

# The outbound coordinator client. libcurl is not a new dependency on an
# FPP host (fppd links it already); it is named here so the adapter links
# it explicitly rather than relying on whatever the host process happens
# to have loaded.
CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)
ifeq ($(strip $(CURL_LIBS)),)
CURL_LIBS := -lcurl
endif

# The FPP 10 adapter declares FPP_PLUGIN_SUPPORTS_UNLOAD(), and that
# declaration is only honest if the library can actually be unmapped. glibc
# permanently marks the first object defining an STB_GNU_UNIQUE symbol
# NODELETE, so dlclose() would succeed and unmap nothing, silently. GCC emits
# that binding for a static local in an inline function, which this tree has
# in CLocaleGuard, reached through the core sources both adapters link. The
# FPP 9 link deliberately does not carry the flag: FPP 9 has no unload path at
# all, so an STB_GNU_UNIQUE binding there is harmless. Appended separately from
# CXXFLAGS so a caller overriding CXXFLAGS cannot drop it. clang does not accept
# the flag and does not emit the binding, so it is gcc-only. verify-fpp10
# asserts the result.
ifeq '$(findstring clang,$(shell $(CXX) --version 2>/dev/null))' ''
FPP10_UNLOAD_FLAGS := -fno-gnu-unique
else
FPP10_UNLOAD_FLAGS :=
endif

.PHONY: fpp9
fpp9: $(BUILD)/fpp9/libshowmesh-fpp9.so

.PHONY: fpp10
fpp10: $(BUILD)/fpp10/libshowmesh-fpp10.so

$(BUILD)/fpp9/libshowmesh-fpp9.so: fpp9/plugin.cpp $(CORE_SOURCES)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -shared $^ $(CURL_LIBS) $(OPENSSL_LIBS) -o $@

$(BUILD)/fpp10/libshowmesh-fpp10.so: fpp10/plugin.cpp $(CORE_SOURCES)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(FPP10_UNLOAD_FLAGS) $(INCLUDES) -shared $^ $(CURL_LIBS) $(OPENSSL_LIBS) -o $@

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
	@unique=$$(nm -D $(BUILD)/fpp10/libshowmesh-fpp10.so | awk '$$2=="u"'); \
	if [ -n "$$unique" ]; then \
		echo "verify-fpp10: library defines STB_GNU_UNIQUE symbols, so glibc would mark it NODELETE and dlclose() would unmap nothing despite FPP_PLUGIN_SUPPORTS_UNLOAD():"; \
		echo "$$unique"; \
		exit 1; \
	fi; \
	echo "verify-fpp10: no STB_GNU_UNIQUE symbols, so the library can actually be unmapped"

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

# Unit tests for the adapter-shared headers that need nothing but jsoncpp
# and libcrypto (section_names.h, callback_fields.h,
# fallback_program_verifier.h, fallback_program_installer.h,
# fallback_program_fetch.h), plus the host-neutral entry-key derivation
# they feed. Deliberately does not need FPP_SRC: the shared headers under
# test never pull in an FPP header, so this runs on a plain dev machine,
# unlike fpp9/fpp10 above. coordinator_config.cpp is here for
# joinUrlPath/CredentialSource, the identical credential and URL
# resolution the observation path already uses (fallback_program_fetch.h
# adds no second resolution path).
TEST_CORE_SOURCES := $(CORE_DIR)/src/json.cpp $(CORE_DIR)/src/sha256.cpp $(CORE_DIR)/src/playlist_identity.cpp \
                      $(CORE_DIR)/src/atomic_write.cpp $(CORE_DIR)/src/coordinator_config.cpp
TEST_SOURCES       := shared/tests/adapter_shared_test.cpp $(CORE_DIR)/tests/main.cpp $(TEST_CORE_SOURCES)
TEST_OBJDIR        := $(BUILD)/test-obj
TEST_OBJECTS       := $(addprefix $(TEST_OBJDIR)/,$(notdir $(TEST_SOURCES:.cpp=.o)))
TEST_BINARY        := $(BUILD)/adapter_shared_tests

vpath %.cpp shared/tests $(CORE_DIR)/tests $(CORE_DIR)/src

# Objects are named by basename only, not by path, because the sources live
# under $(CORE_DIR) (..) and native/Makefile's path-preserving pattern would
# place objects outside $(BUILD) (e.g. $(BUILD)/../src/json.o). Flattening
# means two test sources with the same basename in different directories
# would silently overwrite one object with the other, so that collision is
# turned into a parse-time error instead.
ifneq ($(words $(TEST_OBJECTS)),$(words $(sort $(TEST_OBJECTS))))
$(error two test sources share a basename, so the flattened object directory would silently drop one)
endif

.PHONY: test
test: $(TEST_BINARY)
	$(TEST_BINARY)

# Each source compiles to its own object with -MMD -MP so make learns the
# header set from the compiler instead of a hand-listed prerequisite: a
# hand-listed one goes stale the moment a source gains a new #include and
# nothing notices, which let $(TEST_BINARY) below go stale for years
# against shared/callback_fields.h and shared/section_names.h.
$(TEST_OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(CORE_DIR)/include -I$(CORE_DIR)/tests -Ishared $(JSONCPP_INCLUDE) $(OPENSSL_INCLUDE) -MMD -MP -c $< -o $@

$(TEST_BINARY): $(TEST_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ $(JSONCPP_LIBS) $(OPENSSL_LIBS) -o $@

-include $(TEST_OBJECTS:.o=.d)

.PHONY: clean
clean:
	rm -rf $(BUILD)
