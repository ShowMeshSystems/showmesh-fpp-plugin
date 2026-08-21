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

.PHONY: clean
clean:
	rm -rf $(BUILD)

-include $(CORE_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)
