CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wconversion
CPPFLAGS ?= -Iinclude
LDLIBS ?=

ifeq ($(OS),Windows_NT)
LDLIBS += -lws2_32
endif

.PHONY: all test validate-shaders clean
CORE_SOURCES := src/graphics_shell.cpp src/lan_administrator.cpp src/lan_discovery.cpp src/lan_protocol.cpp src/massive_classifier.cpp

all: build/ein_relay_demo build/ein_relay_tests build/ein_classifier_tests build/ein_lan_tests

build/ein_relay_demo: $(CORE_SOURCES) src/demo.cpp include/ein/graphics_shell.hpp include/ein/massive_classifier.hpp include/ein/lan_protocol.hpp include/ein/lan_discovery.hpp include/ein/lan_administrator.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_SOURCES) src/demo.cpp -o $@ $(LDLIBS)

build/ein_relay_tests: $(CORE_SOURCES) tests/path_relay_tests.cpp include/ein/graphics_shell.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_SOURCES) tests/path_relay_tests.cpp -o $@ $(LDLIBS)

build/ein_classifier_tests: $(CORE_SOURCES) tests/massive_classifier_tests.cpp include/ein/massive_classifier.hpp include/ein/gpu_layout.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_SOURCES) tests/massive_classifier_tests.cpp -o $@ $(LDLIBS)

build/ein_lan_tests: $(CORE_SOURCES) tests/lan_administrator_tests.cpp include/ein/lan_protocol.hpp include/ein/lan_discovery.hpp include/ein/lan_administrator.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_SOURCES) tests/lan_administrator_tests.cpp -o $@ $(LDLIBS)

test: build/ein_relay_demo build/ein_relay_tests build/ein_classifier_tests build/ein_lan_tests
	./build/ein_relay_tests
	./build/ein_classifier_tests
	./build/ein_lan_tests
	./build/ein_relay_demo >/dev/null

validate-shaders:
	sh scripts/validate_shaders.sh

clean:
	rm -f build/ein_relay_demo build/ein_relay_tests build/ein_classifier_tests build/ein_lan_tests
