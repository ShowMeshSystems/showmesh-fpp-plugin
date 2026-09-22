# The host-neutral core. No FPP headers, no third-party libraries: this
# builds and its tests run on any machine with a C++17 compiler, which is
# what makes the engine testable long before an FPP host is involved.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
INCLUDES := -Iinclude
LDFLAGS  ?= -pthread
BUILD    := build

CORE_SOURCES  := $(wildcard src/*.cpp)
TEST_SOURCES  := $(wildcard tests/*.cpp)
CORE_OBJECTS  := $(patsubst %.cpp,$(BUILD)/%.o,$(CORE_SOURCES))
TEST_OBJECTS  := $(patsubst %.cpp,$(BUILD)/%.o,$(TEST_SOURCES))
TEST_BINARY   := $(BUILD)/core_tests
CORE_LIBRARY  := $(BUILD)/libshowmesh_core.a

.PHONY: all
all: $(CORE_LIBRARY)

$(CORE_LIBRARY): $(CORE_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Itests -MMD -MP -c $< -o $@

$(TEST_BINARY): $(CORE_OBJECTS) $(TEST_OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

.PHONY: test
test: $(TEST_BINARY)
	$(TEST_BINARY)

# The engine must never acquire an FPP dependency by accident. A header
# reaching for one would compile here today and fail on a host with a
# different FPP major, which is exactly the coupling the adapters exist to
# contain.
.PHONY: check-host-neutral
check-host-neutral:
	@if grep -rn --include='*.h' --include='*.cpp' -E '#include[[:space:]]*[<"](fpp|FPP|Plugin|MultiSync|channeloutput)' include src tests; then \
		echo "the host-neutral core must not include an FPP header"; exit 1; \
	fi
	@echo "check-host-neutral: OK"

# A diagnostic, not a correctness gate: it prints a disagreement rate
# rather than asserting one, so it stays out of `test`. It exists as a
# make target at all so it cannot silently stop compiling again the next
# time BrightnessEngine's adoptState signature changes; it is not part of
# `all` and native/tools is deliberately absent from the repository root
# Makefile's release-native-bundle git-archive path list.
CONVERGENCE_HARNESS_BINARY := $(BUILD)/convergence_harness

.PHONY: convergence-harness
convergence-harness: $(CONVERGENCE_HARNESS_BINARY)
	$(CONVERGENCE_HARNESS_BINARY)

$(CONVERGENCE_HARNESS_BINARY): tools/convergence_harness.cpp tools/convergence_model.h src/brightness.cpp src/json.cpp src/sha256.cpp include/showmesh/brightness.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Itools tools/convergence_harness.cpp src/brightness.cpp src/json.cpp src/sha256.cpp -o $@

CXXSTD_ADAPTERS ?= -std=c++20

JSONCPP_INCLUDE := $(shell pkg-config --cflags-only-I jsoncpp 2>/dev/null | sed 's/-I/-isystem /g')
ifeq ($(strip $(JSONCPP_INCLUDE)),)
JSONCPP_INCLUDE := -isystem /usr/include/jsoncpp
endif

OPENSSL_INCLUDE := $(shell pkg-config --cflags-only-I libcrypto 2>/dev/null | sed 's/-I/-isystem /g')
ifeq ($(strip $(OPENSSL_INCLUDE)),)
OPENSSL_INCLUDE := -isystem /usr/include
endif

# -fsyntax-only against adapters/syntax_stubs/ (see its README.md):
# neither plugin.cpp is otherwise compiled by anything `make check` runs,
# so a broken include, a typo'd FPP symbol, or a signature drift between
# this repository's own code and the shared adapter headers went
# unnoticed until bench/fpp-plugin-load next ran, or a real release build.
# This is not a substitute for that bench: the stubs describe the shapes
# these two files reference, not the real FPP API. Real jsoncpp headers
# are used, not stubbed, because both real adapter builds already require
# them and the API is too large to usefully fake. No -Werror: the stubs'
# own minimal bodies are not written to satisfy every warning real FPP
# headers happen to silence, and a spurious warning here must never be
# indistinguishable from a real one in the adapter's own code.
.PHONY: adapter-syntax-check
adapter-syntax-check:
	$(CXX) $(CXXSTD_ADAPTERS) -fsyntax-only -Wall -Iinclude -Iadapters/shared \
		-Iadapters/syntax_stubs/common -Iadapters/syntax_stubs/fpp9 \
		$(JSONCPP_INCLUDE) $(OPENSSL_INCLUDE) adapters/fpp9/plugin.cpp
	$(CXX) $(CXXSTD_ADAPTERS) -fsyntax-only -Wall -Iinclude -Iadapters/shared \
		-Iadapters/syntax_stubs/common -Iadapters/syntax_stubs/fpp10 \
		$(JSONCPP_INCLUDE) $(OPENSSL_INCLUDE) adapters/fpp10/plugin.cpp
	$(CXX) $(CXXSTD_ADAPTERS) -fsyntax-only -Wall -Iinclude -Iadapters/shared \
		-Iadapters/syntax_stubs/common adapters/syntax_stubs/probe_coordinator_delivery.cpp
	$(CXX) $(CXXSTD_ADAPTERS) -fsyntax-only -Wall -Iinclude -Iadapters/shared \
		-Iadapters/syntax_stubs/common adapters/syntax_stubs/probe_pairing_delivery.cpp
	@echo "adapter-syntax-check: OK"

.PHONY: clean
clean:
	rm -rf $(BUILD)

-include $(CORE_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)
